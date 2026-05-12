#include "md_gateway/adapter/deribit/deribit_md_ws_client.h"

#include "md_gateway/adapter/deribit/deribit_md_encoder.h"

#include <bpt_common/logging.h>

namespace bpt::md_gateway::adapter {

void DeribitMdWsClient::on_frame(std::string_view payload, uint64_t recv_ns) {
    if (handler_)
        handler_(payload, recv_ns);

    // Service test_request at the cadence of incoming frames — preserves
    // the previous pre-RunLoop latency on active connections.
    send_test_response_if_pending();
}

void DeribitMdWsClient::on_tick() {
    // Answer test_request before sending any subscribes — Deribit tears
    // down the session if test goes unanswered past ~30s, and subscribes
    // could race ahead of the reply.
    send_test_response_if_pending();

    // Fallback drain — primary subscribe path is the adapter's
    // subscribe() override.
    for (const auto& entry : subs_.take_pending())
        send(deribit::build_subscribe_rpc(next_rpc_id(), entry.symbol, entry.depth));
}

void DeribitMdWsClient::send_test_response_if_pending() noexcept {
    if (!needs_test_response_.load(std::memory_order_acquire))
        return;
    needs_test_response_.store(false, std::memory_order_relaxed);
    if (send(deribit::build_test_response_rpc(next_rpc_id())))
        bpt::common::log::debug("DeribitMdWsClient: responded to test_request");
}

}  // namespace bpt::md_gateway::adapter
