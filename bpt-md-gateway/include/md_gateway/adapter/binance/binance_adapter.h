#pragma once

#include "md_gateway/adapter/binance/binance_funding_rate_stream.h"
#include "md_gateway/adapter/binance/binance_parser.h"
#include "md_gateway/adapter/common/adapter_base.h"

#include <atomic>
#include <bpt_common/ws/run_loop.h>

namespace bpt::md_gateway::adapter {

// Binance market-data adapter.
//
// Main WS: wss://stream.binance.com:9443/stream?streams=<sym>@bookTicker/<sym>@aggTrade/...
//   Subscriptions are baked into the URL — runtime subscribe/unsubscribe take
//   effect on the next reconnect.
//
// Funding rate WS runs on its own thread inside BinanceFundingRateStream
// (fstream.binance.com/stream?streams=!markPrice@arr@1s).
//
// Binance uses Beast's WS-level control-frame pings (set in connect_and_subscribe),
// so ping_config is left at the default nullopt — no application ping thread.
class BinanceAdapter : public AdapterBase, private bpt::common::ws::RunLoop {
public:
    explicit BinanceAdapter(const config::AdapterConfig& cfg, std::shared_ptr<messaging::IMdPublisher> md_pub);

    // Lowercases the symbol before registering — Binance stream names are lowercase.
    void subscribe(uint64_t instrument_id, std::string symbol, uint8_t depth = 0) override;

    // Also starts the funding-rate stream.
    void start() override;

    // Also stops the funding-rate stream.
    void stop() override;

    [[nodiscard]] const char* exchange_name() const override { return "BINANCE"; }
    [[nodiscard]] bpt::common::util::LatencyHistogram& decode_latency_hist() noexcept override { return parser_.decode_lat_; }

protected:
    // Returns nullptr if there are no subscriptions yet (run() will retry).
    std::unique_ptr<bpt::common::ws::AnyWsStream> connect_and_subscribe() override;
    void read_loop(bpt::common::ws::AnyWsStream& ws) override;
    void parse_frame(std::string_view payload, uint64_t recv_ns) override;

    void on_frame(std::string_view payload, uint64_t recv_ns) override;

private:
    BinanceParser parser_;
    std::atomic<bool> rl_connected_{false};
    BinanceFundingRateStream fr_stream_;
};

}  // namespace bpt::md_gateway::adapter
