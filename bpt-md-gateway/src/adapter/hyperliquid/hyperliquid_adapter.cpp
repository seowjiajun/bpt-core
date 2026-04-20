#include "md_gateway/adapter/hyperliquid/hyperliquid_adapter.h"

#include "md_gateway/adapter/hyperliquid/hyperliquid_md_encoder.h"

#include <boost/asio/buffer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <bpt_common/ws/ws_connect.h>

namespace bpt::md_gateway::adapter {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;

HyperliquidAdapter::HyperliquidAdapter(const config::AdapterConfig& cfg,
                                       std::shared_ptr<messaging::IMdPublisher> md_pub)
    : AdapterBase(cfg, std::move(md_pub)),
      parser_(subs_) {}

std::unique_ptr<bpt::common::ws::AnyWsStream> HyperliquidAdapter::connect_and_subscribe() {
    bpt::common::log::info("HyperliquidAdapter connecting {}:{}{}", cfg_.ws_host, cfg_.ws_port, cfg_.ws_path);
    auto tls_ws = bpt::common::ws::ws_connect(ioc_,
                                      ssl_ctx_,
                                      cfg_.ws_host,
                                      cfg_.ws_port,
                                      cfg_.ws_path,
                                      cfg_.so_rcvbuf_bytes,
                                      cfg_.ws_connect_timeout_ms,
                                      "bpt-md-gateway/0.1",
                                      cfg_.pinned_tls_sha256);
    auto ws = std::make_unique<bpt::common::ws::AnyWsStream>(std::move(tls_ws));

    // Enable WebSocket-level keep-alive pings. If HL stops responding Beast
    // closes the stream with an error, triggering the reconnect loop.
    // Complements the application-level ping via ping_config + the
    // liveness_timeout watchdog inside RunLoop.
    ws->set_option(websocket::stream_base::timeout{
        websocket::stream_base::none(),  // connect timeout handled in ws_connect
        std::chrono::seconds(30),        // idle timeout before Beast sends a ping
        true                             // send keep-alive ping frames
    });

    bpt::common::log::info("HyperliquidAdapter connected, subscribing instruments");

    // Drain pending so the read loop does not re-send what we subscribe here.
    subs_.take_pending();
    for (const auto& [id, entry] : subs_.snapshot()) {
        for (const char* type : {"l2Book", "trades", "activeAssetCtx"})
            ws->write(net::buffer(hyperliquid::build_subscribe_payload(type, entry.symbol)));
    }

    return ws;
}

void HyperliquidAdapter::read_loop(bpt::common::ws::AnyWsStream& ws) {
    RunLoop::run(std::move(ws),
                 stop_flag_,
                 rl_connected_,
                 std::chrono::milliseconds(cfg_.ws_read_timeout_ms),
                 std::chrono::milliseconds(cfg_.ws_liveness_timeout_ms));
}

void HyperliquidAdapter::on_frame(std::string_view payload, uint64_t recv_ns) {
    push_frame(payload, recv_ns);
}

void HyperliquidAdapter::on_tick() {
    // Fallback for subs added between connect_and_subscribe's take_pending
    // and the first read iteration. Primary path is subscribe() below.
    for (const auto& entry : subs_.take_pending()) {
        for (const char* type : {"l2Book", "trades", "activeAssetCtx"})
            RunLoop::send(hyperliquid::build_subscribe_payload(type, entry.symbol));
        bpt::common::log::info("HyperliquidAdapter: on_tick subscribe {}", entry.symbol);
    }
}

void HyperliquidAdapter::subscribe(uint64_t instrument_id, std::string symbol, uint8_t depth) {
    AdapterBase::subscribe(instrument_id, symbol, depth);
    // Push to the wire immediately when connected. See OkxAdapter::subscribe
    // for the underlying rationale — sync ws.read doesn't time out here, so
    // on_tick is unreliable for runtime subs.
    bool sent = false;
    for (const char* type : {"l2Book", "trades", "activeAssetCtx"}) {
        if (RunLoop::send(hyperliquid::build_subscribe_payload(type, symbol)))
            sent = true;
    }
    if (sent) {
        bpt::common::log::info("HyperliquidAdapter: runtime subscribe {}", symbol);
        subs_.take_pending();  // don't double-send in on_tick
    }
}

std::optional<bpt::common::ws::PingConfig> HyperliquidAdapter::ping_config() const {
    // Hyperliquid closes idle WebSockets ~60s after the last client-sent
    // message, so a 20s application ping keeps the session alive even
    // when market data goes quiet.
    return bpt::common::ws::PingConfig{
        std::chrono::seconds(20),
        [] { return hyperliquid::build_ping_payload(); },
    };
}

void HyperliquidAdapter::parse_frame(std::string_view payload, uint64_t recv_ns) {
    parser_.parse(payload, recv_ns, validating_pub_, on_funding_rate);
}

}  // namespace bpt::md_gateway::adapter
