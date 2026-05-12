#include "md_gateway/adapter/hyperliquid/hyperliquid_md_ws_client.h"

#include "md_gateway/adapter/hyperliquid/hyperliquid_md_encoder.h"

#include <bpt_common/logging.h>
#include <chrono>

namespace bpt::md_gateway::adapter {

void HyperliquidMdWsClient::on_tick() {
    // Fallback for subs added between connect_and_subscribe's take_pending
    // and the first read iteration. Primary path is the adapter's subscribe().
    for (const auto& entry : subs_.take_pending()) {
        for (const char* type : {"l2Book", "trades", "activeAssetCtx"})
            send(hyperliquid::build_subscribe_payload(type, entry.symbol));
        bpt::common::log::info("HyperliquidMdWsClient: on_tick subscribe {}", entry.symbol);
    }
}

std::optional<bpt::common::ws::PingConfig> HyperliquidMdWsClient::ping_config() const {
    // Hyperliquid closes idle WebSockets ~60s after the last client-sent
    // message, so a 20s application ping keeps the session alive even
    // when market data goes quiet.
    (void)cfg_;
    return bpt::common::ws::PingConfig{
        std::chrono::seconds(20),
        [] { return hyperliquid::build_ping_payload(); },
    };
}

}  // namespace bpt::md_gateway::adapter
