#include "md_gateway/adapter/deribit/deribit_adapter.h"

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

DeribitAdapter::DeribitAdapter(const config::AdapterConfig& cfg, std::shared_ptr<messaging::IMdPublisher> md_pub)
    : AdapterBase(cfg, std::move(md_pub)),
      parser_(subs_) {}

void DeribitAdapter::unsubscribe(uint64_t instrument_id) {
    std::string symbol = subs_.unsubscribe(instrument_id);
    if (!symbol.empty())
        parser_.forget(symbol);
}

std::chrono::milliseconds DeribitAdapter::reconnect_delay() const {
    return std::chrono::seconds(2);
}

void DeribitAdapter::send_subscribe_rpc(bpt::common::ws::AnyWsStream& ws, const std::string& symbol, uint8_t depth) {
    const std::string book_channel =
        (depth == 0) ? fmt::format("quote.{}", symbol) : fmt::format("book.{}.100ms", symbol);

    auto req = fmt::format(
        R"({{"jsonrpc":"2.0","id":{},"method":"public/subscribe","params":{{"channels":["trades.{}.100ms","{}"]}}}})",
        rpc_id_.fetch_add(1, std::memory_order_relaxed),
        symbol,
        book_channel);
    ws.write(net::buffer(req));

    bpt::common::log::info("DeribitAdapter: subscribed {} depth={}", symbol, depth);
}

void DeribitAdapter::send_test_response(bpt::common::ws::AnyWsStream& ws) {
    auto resp = fmt::format(R"({{"jsonrpc":"2.0","id":{},"method":"public/test","params":{{}}}})",
                            rpc_id_.fetch_add(1, std::memory_order_relaxed));
    ws.write(net::buffer(resp));
    bpt::common::log::debug("DeribitAdapter: responded to test_request");
}

std::unique_ptr<bpt::common::ws::AnyWsStream> DeribitAdapter::connect_and_subscribe() {
    bpt::common::log::info("DeribitAdapter connecting {}:{}{}", cfg_.ws_host, cfg_.ws_port, cfg_.ws_path);
    auto tls_ws = bpt::common::ws::ws_connect(ioc_,
                                      ssl_ctx_,
                                      cfg_.ws_host,
                                      cfg_.ws_port,
                                      cfg_.ws_path,
                                      cfg_.so_rcvbuf_bytes,
                                      cfg_.ws_connect_timeout_ms);
    auto ws = std::make_unique<bpt::common::ws::AnyWsStream>(std::move(tls_ws));

    ws->text(true);
    ws->set_option(websocket::stream_base::timeout{
        websocket::stream_base::none(),  // connect timeout handled in ws_connect
        websocket::stream_base::none(),  // no idle timeout — Deribit uses JSON-RPC heartbeat
        false                            // no Beast keep-alive pings
    });

    bpt::common::log::info("DeribitAdapter connected");

    // Enable Deribit heartbeat — CRITICAL: Deribit disconnects within 30s if
    // test_request is not answered with public/test.
    {
        auto hb_req =
            fmt::format(R"({{"jsonrpc":"2.0","id":{},"method":"public/set_heartbeat","params":{{"interval":30}}}})",
                        rpc_id_.fetch_add(1, std::memory_order_relaxed));
        ws->write(net::buffer(hb_req));
        bpt::common::log::info("DeribitAdapter: heartbeat enabled (interval=30s)");
    }

    // Clear stale order book gap state before receiving new snapshots.
    parser_.reset();

    // Drain pending so the read loop does not re-send what we're about to subscribe.
    subs_.take_pending();

    for (const auto& [id, entry] : subs_.snapshot())
        send_subscribe_rpc(*ws, entry.symbol, entry.depth);

    return ws;
}

void DeribitAdapter::read_loop(bpt::common::ws::AnyWsStream& ws) {
    beast::flat_buffer buf;
    const auto liveness = std::chrono::milliseconds(cfg_.ws_liveness_timeout_ms);
    auto last_recv = std::chrono::steady_clock::now();

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        // Reset timer first — covers all writes below (test_request response,
        // subscribe frames) as well as the read.  A prior read timeout leaves
        // the timer expired; any write before the next expires_after would fail.
        ws.expires_after(std::chrono::milliseconds(cfg_.ws_read_timeout_ms));

        // Respond to heartbeat test_request if the publisher thread flagged one.
        // The WS write must happen on the IO thread that owns the bpt::common::ws::WsStream.
        if (needs_test_response_.load(std::memory_order_acquire)) {
            needs_test_response_.store(false, std::memory_order_relaxed);
            send_test_response(ws);
        }

        // Send subscribe frames for any instruments added since connect.
        for (const auto& entry : subs_.take_pending())
            send_subscribe_rpc(ws, entry.symbol, entry.depth);

        beast::error_code ec;
        ws.read(buf, ec);
        if (ec == beast::error::timeout) {
            buf.consume(buf.size());
            if (std::chrono::steady_clock::now() - last_recv >= liveness) {
                bpt::common::log::warn("DeribitAdapter: no data for {}ms, reconnecting", cfg_.ws_liveness_timeout_ms);
                throw std::runtime_error("liveness timeout");
            }
            continue;
        }
        if (ec)
            throw beast::system_error(ec);

        last_recv = std::chrono::steady_clock::now();
        // WallClock, not TscClock — this timestamp crosses a process boundary
        // (bpt-md-gateway → fenrir via Aeron SBE) and would suffer from per-process
        // TscClock calibration drift. See HyperliquidAdapter for details.
        uint64_t recv_ns = bpt::common::util::WallClock::now_ns();
        std::string_view msg(static_cast<const char*>(buf.data().data()), buf.data().size());
        push_frame(msg, recv_ns);
        buf.consume(buf.size());
    }

    ws.close(websocket::close_code::normal);
}

void DeribitAdapter::parse_frame(std::string_view payload, uint64_t recv_ns) {
    parser_.parse(payload, recv_ns, validating_pub_, on_funding_rate);
    if (parser_.take_test_request())
        needs_test_response_.store(true, std::memory_order_release);
}

}  // namespace bpt::md_gateway::adapter
