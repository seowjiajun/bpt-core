#include "md_gateway/adapter/okx/okx_md_ws_client.h"

#include "md_gateway/adapter/okx/okx_md_encoder.h"

#include <bpt_common/logging.h>
#include <chrono>
#include <string>

namespace bpt::md_gateway::adapter {

void OkxMdWsClient::on_frame(std::string_view payload, uint64_t recv_ns) {
    // OKX's app-level heartbeat is bidirectional: the exchange may send
    // "ping" expecting a "pong" reply, and we send "ping" via ping_config.
    // Don't push keepalive text frames onto the parser queue.
    if (payload == "ping") {
        send("pong");
        return;
    }
    if (payload == "pong")
        return;

    if (handler_)
        handler_(payload, recv_ns);
}

void OkxMdWsClient::on_tick() {
    // Fallback for subs added between connect and the immediate-send path
    // in the adapter. Normal path: adapter::subscribe() sends immediately
    // via send() when connected, so this typically finds nothing pending.
    for (const auto& entry : subs_.take_pending()) {
        if (send(okx::build_subscribe_payload(entry.symbol, entry.depth)))
            bpt::common::log::info("OkxMdWsClient: on_tick subscribe {} depth={}", entry.symbol, entry.depth);
    }
}

std::optional<bpt::common::ws::PingConfig> OkxMdWsClient::ping_config() const {
    return bpt::common::ws::PingConfig{
        std::chrono::milliseconds(cfg_.ws_ping_interval_ms),
        [] { return std::string("ping"); },
    };
}

}  // namespace bpt::md_gateway::adapter
