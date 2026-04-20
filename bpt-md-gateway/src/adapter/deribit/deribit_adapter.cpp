#include "md_gateway/adapter/deribit/deribit_adapter.h"

#include "md_gateway/adapter/deribit/deribit_md_encoder.h"

#include <boost/asio/buffer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
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

std::unique_ptr<bpt::common::ws::AnyWsStream> DeribitAdapter::connect_and_subscribe() {
    bpt::common::log::info("DeribitAdapter connecting {}:{}{}", cfg_.ws_host, cfg_.ws_port, cfg_.ws_path);
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

    ws->text(true);
    ws->set_option(websocket::stream_base::timeout{
        websocket::stream_base::none(),  // connect timeout handled in ws_connect
        websocket::stream_base::none(),  // no idle timeout — Deribit uses JSON-RPC heartbeat
        false                            // no Beast keep-alive pings
    });

    bpt::common::log::info("DeribitAdapter connected");

    // Enable Deribit heartbeat — CRITICAL: Deribit disconnects within 30s if
    // test_request is not answered with public/test.
    ws->write(net::buffer(deribit::build_set_heartbeat_rpc(
        rpc_id_.fetch_add(1, std::memory_order_relaxed), /*interval_s=*/30)));
    bpt::common::log::info("DeribitAdapter: heartbeat enabled (interval=30s)");

    // Clear stale order book gap state before receiving new snapshots.
    parser_.reset();

    // Drain pending so the read loop does not re-send what we subscribe here.
    subs_.take_pending();
    for (const auto& [id, entry] : subs_.snapshot())
        ws->write(net::buffer(deribit::build_subscribe_rpc(
            rpc_id_.fetch_add(1, std::memory_order_relaxed), entry.symbol, entry.depth)));

    return ws;
}

void DeribitAdapter::read_loop(bpt::common::ws::AnyWsStream& ws) {
    RunLoop::run(std::move(ws),
                 stop_flag_,
                 rl_connected_,
                 std::chrono::milliseconds(cfg_.ws_read_timeout_ms),
                 std::chrono::milliseconds(cfg_.ws_liveness_timeout_ms));
}

void DeribitAdapter::on_frame(std::string_view payload, uint64_t recv_ns) {
    push_frame(payload, recv_ns);

    // Also check here so test_request gets answered at the cadence of the
    // incoming frames instead of waiting for the next read_timeout tick —
    // preserves the previous pre-RunLoop latency on active connections.
    if (needs_test_response_.load(std::memory_order_acquire)) {
        needs_test_response_.store(false, std::memory_order_relaxed);
        if (RunLoop::send(deribit::build_test_response_rpc(rpc_id_.fetch_add(1, std::memory_order_relaxed))))
            bpt::common::log::debug("DeribitAdapter: responded to test_request");
    }
}

void DeribitAdapter::on_tick() {
    // Answer a test_request from the parser thread before sending any
    // subscribes — Deribit tears down the session if test goes unanswered
    // past ~30s, and subscribes could race ahead of the reply. Also runs
    // on_frame's identical check so the response fires whichever path
    // observes the flag first.
    if (needs_test_response_.load(std::memory_order_acquire)) {
        needs_test_response_.store(false, std::memory_order_relaxed);
        if (RunLoop::send(deribit::build_test_response_rpc(rpc_id_.fetch_add(1, std::memory_order_relaxed))))
            bpt::common::log::debug("DeribitAdapter: responded to test_request");
    }

    // Fallback drain — primary subscribe path is the subscribe() override below.
    for (const auto& entry : subs_.take_pending())
        RunLoop::send(deribit::build_subscribe_rpc(
            rpc_id_.fetch_add(1, std::memory_order_relaxed), entry.symbol, entry.depth));
}

void DeribitAdapter::subscribe(uint64_t instrument_id, std::string symbol, uint8_t depth) {
    AdapterBase::subscribe(instrument_id, symbol, depth);
    // Push to the wire immediately when connected. See OkxAdapter::subscribe
    // for the underlying rationale.
    if (RunLoop::send(deribit::build_subscribe_rpc(
            rpc_id_.fetch_add(1, std::memory_order_relaxed), symbol, depth))) {
        bpt::common::log::info("DeribitAdapter: runtime subscribe {} depth={}", symbol, depth);
        subs_.take_pending();
    }
}

void DeribitAdapter::parse_frame(std::string_view payload, uint64_t recv_ns) {
    parser_.parse(payload, recv_ns, validating_pub_, on_funding_rate);
    if (parser_.take_test_request())
        needs_test_response_.store(true, std::memory_order_release);
}

}  // namespace bpt::md_gateway::adapter
