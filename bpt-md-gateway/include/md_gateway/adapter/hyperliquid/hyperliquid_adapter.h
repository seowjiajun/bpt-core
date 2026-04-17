#pragma once

#include "md_gateway/adapter/common/adapter_base.h"
#include "md_gateway/adapter/hyperliquid/hyperliquid_parser.h"

namespace bpt::md_gateway::adapter {

// Hyperliquid market-data adapter.
//
// Connects to wss://api.hyperliquid.xyz/ws.
// Subscribes to l2Book, trades, and activeAssetCtx per instrument.
// Runtime subscribe/unsubscribe take effect immediately via the pending queue.
class HyperliquidAdapter : public AdapterBase {
public:
    explicit HyperliquidAdapter(const config::AdapterConfig& cfg, std::shared_ptr<messaging::IMdPublisher> md_pub);

    [[nodiscard]] const char* exchange_name() const override { return "HYPERLIQUID"; }
    [[nodiscard]] ygg::util::LatencyHistogram& decode_latency_hist() noexcept override { return parser_.decode_lat_; }

protected:
    std::unique_ptr<ygg::ws::AnyWsStream> connect_and_subscribe() override;
    void read_loop(ygg::ws::AnyWsStream& ws) override;
    void parse_frame(std::string_view payload, uint64_t recv_ns) override;

private:
    void send_instrument_subs(ygg::ws::AnyWsStream& ws, const std::string& coin);

    HyperliquidParser parser_;
};

}  // namespace bpt::md_gateway::adapter
