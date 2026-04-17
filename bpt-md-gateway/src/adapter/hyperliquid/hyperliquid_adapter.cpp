#include "md_gateway/adapter/hyperliquid/hyperliquid_adapter.h"

#include <boost/asio/buffer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <fmt/format.h>
#include <yggdrasil/util/tsc_clock.h>
#include <yggdrasil/ws/ws_connect.h>

namespace bpt::md_gateway::adapter {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;

HyperliquidAdapter::HyperliquidAdapter(const config::AdapterConfig& cfg,
                                       std::shared_ptr<messaging::IMdPublisher> md_pub)
    : AdapterBase(cfg, std::move(md_pub)),
      parser_(subs_) {}

void HyperliquidAdapter::send_instrument_subs(ygg::ws::AnyWsStream& ws, const std::string& coin) {
    for (const char* type : {"l2Book", "trades", "activeAssetCtx"}) {
        auto sub = fmt::format(R"({{"method":"subscribe","subscription":{{"type":"{}","coin":"{}"}}}})", type, coin);
        ws.write(net::buffer(sub));
    }
}

std::unique_ptr<ygg::ws::AnyWsStream> HyperliquidAdapter::connect_and_subscribe() {
    ygg::log::info("HyperliquidAdapter connecting {}:{}{}", cfg_.ws_host, cfg_.ws_port, cfg_.ws_path);
    auto tls_ws = ygg::ws::ws_connect(ioc_,
                                      ssl_ctx_,
                                      cfg_.ws_host,
                                      cfg_.ws_port,
                                      cfg_.ws_path,
                                      cfg_.so_rcvbuf_bytes,
                                      cfg_.ws_connect_timeout_ms);
    auto ws = std::make_unique<ygg::ws::AnyWsStream>(std::move(tls_ws));

    // Enable WebSocket-level keep-alive pings. If HL stops responding Beast
    // closes the stream with an error, triggering the reconnect loop.
    // Complements the application-level last_recv liveness check in read_loop.
    ws->set_option(websocket::stream_base::timeout{
        websocket::stream_base::none(),  // connect timeout handled in ws_connect
        std::chrono::seconds(30),        // idle timeout before Beast sends a ping
        true                             // send keep-alive ping frames
    });

    ygg::log::info("HyperliquidAdapter connected, subscribing instruments");

    // Drain pending so the read loop does not re-send what we're about to subscribe.
    subs_.take_pending();

    for (const auto& [id, entry] : subs_.snapshot())
        send_instrument_subs(*ws, entry.symbol);

    return ws;
}

void HyperliquidAdapter::read_loop(ygg::ws::AnyWsStream& ws) {
    beast::flat_buffer buf;
    const auto liveness = std::chrono::milliseconds(cfg_.ws_liveness_timeout_ms);
    auto last_recv = std::chrono::steady_clock::now();

    // Hyperliquid closes idle WebSockets ~60s after the last client-sent
    // message. Beast's get_lowest_layer().expires_after() doesn't reliably
    // bound a multi-frame ws.read() — its timer is consumed by the first
    // TCP op and the read can keep going. So an in-loop ping check after
    // ws.read() can sit blocked for a full 60s, missing every ping window.
    //
    // Solution: dedicated ping thread that writes JSON pings every 20s.
    // Beast websocket::stream supports concurrent read+write across threads
    // as long as each direction is single-threaded.
    std::atomic<bool> ping_stop{false};
    std::thread ping_thread([&] {
        while (!ping_stop.load(std::memory_order_relaxed)) {
            for (int i = 0; i < 20 && !ping_stop.load(std::memory_order_relaxed); ++i)
                std::this_thread::sleep_for(std::chrono::seconds(1));
            if (ping_stop.load(std::memory_order_relaxed))
                break;
            try {
                static const std::string msg = R"({"method":"ping"})";
                ws.write(net::buffer(msg));
                ygg::log::info("HyperliquidAdapter: ping sent");
            } catch (const std::exception& e) {
                ygg::log::warn("HyperliquidAdapter: ping write failed: {}", e.what());
                break;
            }
        }
    });
    struct JoinGuard {
        std::atomic<bool>& stop;
        std::thread& th;
        ~JoinGuard() {
            stop.store(true, std::memory_order_relaxed);
            if (th.joinable()) th.join();
        }
    } join_guard{ping_stop, ping_thread};

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        // Send subscribe frames for any instruments added since connect.
        // Note: this does a write on the same WS the ping thread writes to,
        // but runtime subscriptions are rare and bursty — accept the small
        // race for now. A future refactor can guard with a write mutex.
        for (const auto& entry : subs_.take_pending()) {
            send_instrument_subs(ws, entry.symbol);
            ygg::log::info("HyperliquidAdapter: runtime subscribe {}", entry.symbol);
        }

        beast::error_code ec;
        ws.read(buf, ec);

        if (ec == beast::error::timeout) {
            buf.consume(buf.size());
            if (std::chrono::steady_clock::now() - last_recv >= liveness) {
                ygg::log::warn("HyperliquidAdapter: no data for {}ms, reconnecting", cfg_.ws_liveness_timeout_ms);
                throw std::runtime_error("liveness timeout");
            }
            continue;
        }
        if (ec)
            throw beast::system_error(ec);

        last_recv = std::chrono::steady_clock::now();
        // WallClock (not TscClock): this timestamp is serialized into the
        // SBE MD message and compared by downstream services (fenrir) on
        // receipt. TscClock calibration is per-process and drifts across
        // services, which manifests as uint64 underflow in fenrir's
        // tick→strategy latency histogram.
        uint64_t recv_ns = ygg::util::WallClock::now_ns();
        push_frame(std::string_view(static_cast<const char*>(buf.data().data()), buf.data().size()), recv_ns);
        buf.consume(buf.size());
    }
    ws.close(websocket::close_code::normal);
}

void HyperliquidAdapter::parse_frame(std::string_view payload, uint64_t recv_ns) {
    parser_.parse(payload, recv_ns, validating_pub_, on_funding_rate);
}

}  // namespace bpt::md_gateway::adapter
