#pragma once

#include "md_gateway/adapter/common/adapter_base.h"
#include "md_gateway/adapter/hyperliquid/hyperliquid_parser.h"

#include <atomic>
#include <bpt_common/ws/run_loop.h>

namespace bpt::md_gateway::adapter {

// Hyperliquid market-data adapter.
//
// Connects to wss://api.hyperliquid.xyz/ws.
// Subscribes to l2Book, trades, and activeAssetCtx per instrument.
// Runtime subscribe/unsubscribe take effect immediately via the pending queue.
//
// Hyperliquid closes idle WebSockets ~60s after the last client-sent
// message, so ping_config emits a JSON ping on a 20s cadence.
class HyperliquidAdapter : public AdapterBase, private bpt::common::ws::RunLoop {
public:
    explicit HyperliquidAdapter(const config::AdapterConfig& cfg, std::shared_ptr<messaging::IMdPublisher> md_pub);

    [[nodiscard]] const char* exchange_name() const override { return "HYPERLIQUID"; }
    [[nodiscard]] bpt::common::util::LatencyHistogram& decode_latency_hist() noexcept override { return parser_.decode_lat_; }

    // Push subscribe frames to the WS immediately when connected —
    // on_tick can't be relied upon because RunLoop's sync ws.read()
    // doesn't honour expires_after in this Beast version (see OKX
    // adapter commit for the investigation). Same pattern.
    void subscribe(uint64_t instrument_id, std::string symbol, uint8_t depth = 0) override;

protected:
    std::unique_ptr<bpt::common::ws::AnyWsStream> connect_and_subscribe() override;
    void read_loop(bpt::common::ws::AnyWsStream& ws) override;
    void parse_frame(std::string_view payload, uint64_t recv_ns) override;

    void on_frame(std::string_view payload, uint64_t recv_ns) override;
    void on_tick() override;
    std::optional<bpt::common::ws::PingConfig> ping_config() const override;

private:
    HyperliquidParser parser_;
    std::atomic<bool> rl_connected_{false};
};

}  // namespace bpt::md_gateway::adapter
