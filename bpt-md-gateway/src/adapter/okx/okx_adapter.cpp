#include "md_gateway/adapter/okx/okx_adapter.h"

#include "md_gateway/adapter/okx/okx_md_encoder.h"

#include <boost/asio/buffer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <bpt_common/ws/ws_connect.h>

namespace bpt::md_gateway::adapter {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;

OkxAdapter::OkxAdapter(const config::AdapterConfig& cfg, std::shared_ptr<messaging::IMdPublisher> md_pub, const config::RecordingConfig& recording)
    : AdapterBase(cfg, std::move(md_pub), recording),
      parser_(subs_) {}

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
                                      "bpt-md-gateway/0.1",
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

    // OKX requires text frames; Beast defaults to binary. Set here (not
    // in RunLoop::run) so initial subscribe writes below use text mode.
    any->text(true);

    // OKX keepalive must use text-frame "ping" messages, not WebSocket control
    // pings — disable Beast's built-in pings to prevent silent disconnects.
    // The RunLoop ping thread (see ping_config) sends the text pings.
    //
    // idle_timeout left at a positive value because setting it to none()
    // appears to also nullify RunLoop's per-read `expires_after(read_timeout)`
    // in this Beast version, causing on_tick to never fire and runtime-added
    // subscriptions to never reach the WS. A long idle (60s) is plenty given
    // we have our own ping thread + liveness watchdog above it.
    any->set_option(websocket::stream_base::timeout{
        websocket::stream_base::none(),           // connect timeout handled in ws_connect
        std::chrono::seconds(60),                 // idle ≥ max tolerable silence before escalation
        false                                     // no Beast keep-alive pings
    });

    bpt::common::log::info("OkxAdapter connected, subscribing instruments");

    // Drain pending so the read loop does not re-send what we subscribe here.
    subs_.take_pending();
    for (const auto& [id, entry] : subs_.snapshot())
        any->write(net::buffer(okx::build_subscribe_payload(entry.symbol, entry.depth)));

    return any;
}

void OkxAdapter::read_loop(bpt::common::ws::AnyWsStream& ws) {
    // Ownership of ws transfers to RunLoop for the duration of the session.
    // read_timeout controls on_tick cadence + shutdown responsiveness;
    // liveness_timeout escalates a silent connection to a reconnect.
    RunLoop::run(std::move(ws),
                 stop_flag_,
                 rl_connected_,
                 std::chrono::milliseconds(cfg_.ws_read_timeout_ms),
                 std::chrono::milliseconds(cfg_.ws_liveness_timeout_ms));
}

void OkxAdapter::on_frame(std::string_view payload, uint64_t recv_ns) {
    record_raw(payload, recv_ns);
    // OKX's app-level heartbeat is bidirectional: the exchange may send
    // "ping" expecting a "pong" reply, and we send "ping" via ping_config.
    // Don't push keepalive text frames onto the parser queue.
    if (payload == "ping") {
        RunLoop::send("pong");
        return;
    }
    if (payload == "pong")
        return;

    push_frame(payload, recv_ns);
}

void OkxAdapter::on_tick() {
    maybe_checkpoint();
    // Fallback for subs added between connect and the first send below.
    // Normal path: OkxAdapter::subscribe() sends immediately via RunLoop::send
    // when connected, so this typically finds nothing pending.
    for (const auto& entry : subs_.take_pending()) {
        if (RunLoop::send(okx::build_subscribe_payload(entry.symbol, entry.depth)))
            bpt::common::log::info("OkxAdapter: on_tick subscribe {} depth={}", entry.symbol, entry.depth);
    }
}

void OkxAdapter::subscribe(uint64_t instrument_id, std::string symbol, uint8_t depth) {
    // Push through the base class so subs_ tracks the state (connect-time
    // replay, requeue after disconnect, etc).
    AdapterBase::subscribe(instrument_id, symbol, depth);

    // Flush the new subscription to the OKX WS immediately if we're
    // connected. RunLoop::send returns false when ws_ is null (between
    // reconnects); the frame will be picked up by connect_and_subscribe
    // at the next reconnect via subs_.snapshot().
    //
    // This bypasses the on_tick fallback, which in this Beast version
    // can sit in ws.read() indefinitely — ws.expires_after() doesn't
    // time out sync reads, so on_tick only fires when the WS is
    // literally silent for the full read_timeout, which doesn't happen
    // while OKX responds to our ping thread. Runtime subscribes would
    // otherwise never reach the wire.
    if (RunLoop::send(okx::build_subscribe_payload(symbol, depth))) {
        bpt::common::log::info("OkxAdapter: runtime subscribe {} depth={}", symbol, depth);
        // Drain pending to avoid on_tick double-sending this entry.
        subs_.take_pending();
    }
}

std::optional<bpt::common::ws::PingConfig> OkxAdapter::ping_config() const {
    return bpt::common::ws::PingConfig{
        std::chrono::milliseconds(cfg_.ws_ping_interval_ms),
        [] { return std::string("ping"); },
    };
}

void OkxAdapter::parse_frame(std::string_view payload, uint64_t recv_ns) {
    parser_.parse(payload, recv_ns, validating_pub_, on_funding_rate);
}

}  // namespace bpt::md_gateway::adapter
