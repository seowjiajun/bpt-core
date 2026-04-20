#include "md_gateway/adapter/okx/okx_adapter.h"

#include <boost/asio/buffer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <fmt/format.h>
#include <bpt_common/util/tsc_clock.h>
#include <bpt_common/ws/ws_connect.h>

namespace bpt::md_gateway::adapter {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;

OkxAdapter::OkxAdapter(const config::AdapterConfig& cfg, std::shared_ptr<messaging::IMdPublisher> md_pub)
    : AdapterBase(cfg, std::move(md_pub)),
      parser_(subs_) {}

void OkxAdapter::send_instrument_subs(bpt::common::ws::AnyWsStream& ws, const std::string& symbol, uint8_t depth) {
    // Channel selection:
    //   depth 0  → bbo-tbt (tick-by-tick BBO)
    //   depth ≤5 → books5  (top-5 levels)
    //   depth >5 → books   (full depth, 400ms push)
    const char* book_channel = (depth == 0) ? "bbo-tbt" : (depth <= 5) ? "books5" : "books";

    auto sub = fmt::format(
        R"({{"op":"subscribe","args":[{{"channel":"{}","instId":"{}"}},{{"channel":"trades","instId":"{}"}}]}})",
        book_channel,
        symbol,
        symbol);
    ws.write(net::buffer(sub));

    // Subscribe to funding-rate channel for perpetual swaps
    if (symbol.size() > 5 && symbol.substr(symbol.size() - 5) == "-SWAP") {
        auto fr_sub =
            fmt::format(R"({{"op":"subscribe","args":[{{"channel":"funding-rate","instId":"{}"}}]}})", symbol);
        ws.write(net::buffer(fr_sub));
    }
}

std::unique_ptr<bpt::common::ws::AnyWsStream> OkxAdapter::connect_and_subscribe() {
    bpt::common::log::info("OkxAdapter connecting {}:{}{} (tls={})", cfg_.ws_host, cfg_.ws_port, cfg_.ws_path, cfg_.use_tls);

    std::unique_ptr<bpt::common::ws::AnyWsStream> any;
    if (cfg_.use_tls) {
        auto ws = bpt::common::ws::ws_connect(ioc_,
                                      ssl_ctx_,
                                      cfg_.ws_host,
                                      cfg_.ws_port,
                                      cfg_.ws_path,
                                      cfg_.so_rcvbuf_bytes,
                                      cfg_.ws_connect_timeout_ms,
                                      "bpt-client/0.1",
                                      cfg_.pinned_tls_sha256);
        any = std::make_unique<bpt::common::ws::AnyWsStream>(std::move(ws));
    } else {
        auto ws = bpt::common::ws::ws_connect_plain(ioc_,
                                            cfg_.ws_host,
                                            cfg_.ws_port,
                                            cfg_.ws_path,
                                            cfg_.so_rcvbuf_bytes,
                                            cfg_.ws_connect_timeout_ms);
        any = std::make_unique<bpt::common::ws::AnyWsStream>(std::move(ws));
    }

    // OKX requires text frames; Beast defaults to binary.
    any->text(true);

    // OKX keepalive must use text-frame "ping" messages, not WebSocket control
    // pings — disable Beast's built-in pings to prevent silent disconnects.
    any->set_option(websocket::stream_base::timeout{
        websocket::stream_base::none(),  // connect timeout handled in ws_connect
        websocket::stream_base::none(),  // no idle timeout — managed manually via ping/liveness
        false                            // no Beast keep-alive pings
    });

    bpt::common::log::info("OkxAdapter connected, subscribing instruments");

    // Drain pending so the read loop does not re-send what we're about to subscribe.
    subs_.take_pending();

    for (const auto& [id, entry] : subs_.snapshot())
        send_instrument_subs(*any, entry.symbol, entry.depth);

    return any;
}

void OkxAdapter::read_loop(bpt::common::ws::AnyWsStream& ws) {
    const auto ping_interval = std::chrono::milliseconds(cfg_.ws_ping_interval_ms);
    const auto liveness = std::chrono::milliseconds(cfg_.ws_liveness_timeout_ms);

    auto last_ping = std::chrono::steady_clock::now();
    auto last_recv = std::chrono::steady_clock::now();
    beast::flat_buffer buf;

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        // Reset the timer at the top of every iteration so all I/O in this
        // iteration (ping write, subscribe frames, the read itself) is covered.
        // If expires_after were set after the writes, a stale expired timer from
        // the previous iteration's read timeout would cause the first write to
        // fail immediately with beast::error::timeout.
        ws.expires_after(std::chrono::milliseconds(cfg_.ws_read_timeout_ms));

        auto now = std::chrono::steady_clock::now();

        // Send text-frame "ping" on schedule to keep OKX connection alive.
        if (now - last_ping >= ping_interval) {
            ws.write(net::buffer(std::string("ping")));
            last_ping = now;
        }

        // If no message (including pong) has arrived within the liveness window,
        // the exchange is no longer responding — reconnect.
        if (now - last_recv >= liveness) {
            bpt::common::log::warn("OkxAdapter: no data for {}ms, reconnecting", cfg_.ws_liveness_timeout_ms);
            throw std::runtime_error("liveness timeout");
        }

        // Send subscribe frames for any instruments added since connect.
        for (const auto& entry : subs_.take_pending()) {
            send_instrument_subs(ws, entry.symbol, entry.depth);
            bpt::common::log::info("OkxAdapter: runtime subscribe {} depth={}", entry.symbol, entry.depth);
        }

        beast::error_code ec;
        ws.read(buf, ec);
        if (ec == beast::error::timeout) {
            buf.consume(buf.size());
            continue;
        }
        if (ec)
            throw beast::system_error(ec);

        last_recv = std::chrono::steady_clock::now();

        // See HyperliquidAdapter for the WallClock vs TscClock rationale:
        // this timestamp is serialized into SBE MD messages and compared
        // across process boundaries, so it has to be from CLOCK_REALTIME.
        uint64_t recv_ns = bpt::common::util::WallClock::now_ns();
        std::string_view msg(static_cast<const char*>(buf.data().data()), buf.data().size());

        if (msg == "ping") {
            ws.write(net::buffer(std::string("pong")));
        } else if (msg != "pong") {
            push_frame(msg, recv_ns);
        }

        buf.consume(buf.size());
    }

    ws.close(websocket::close_code::normal);
}

void OkxAdapter::parse_frame(std::string_view payload, uint64_t recv_ns) {
    parser_.parse(payload, recv_ns, validating_pub_, on_funding_rate);
}

}  // namespace bpt::md_gateway::adapter
