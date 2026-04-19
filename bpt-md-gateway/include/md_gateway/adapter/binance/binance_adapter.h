#pragma once

#include "md_gateway/adapter/binance/binance_parser.h"
#include "md_gateway/adapter/common/adapter_base.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <simdjson.h>
#include <string>
#include <thread>
#include <vector>

namespace bpt::md_gateway::adapter {

// Binance market-data adapter.
//
// Main WS: wss://stream.binance.com:9443/stream?streams=<sym>@bookTicker/<sym>@aggTrade/...
//   Subscriptions are baked into the URL — runtime subscribe/unsubscribe take
//   effect on the next reconnect.
//
// Funding rate WS (separate thread): fstream.binance.com/stream?streams=!markPrice@arr@1s
//   Publishes FundingRateUpdate for all subscribed instruments found in the stream.
class BinanceAdapter : public AdapterBase {
public:
    explicit BinanceAdapter(const config::AdapterConfig& cfg, std::shared_ptr<messaging::IMdPublisher> md_pub);

    // Lowercases the symbol before registering — Binance stream names are lowercase.
    void subscribe(uint64_t instrument_id, std::string symbol, uint8_t depth = 0) override;

    // Also starts the funding-rate thread.
    void start() override;

    // Also stops the funding-rate thread.
    void stop() override;

    [[nodiscard]] const char* exchange_name() const override { return "BINANCE"; }
    [[nodiscard]] bpt::common::util::LatencyHistogram& decode_latency_hist() noexcept override { return parser_.decode_lat_; }

protected:
    // Returns nullptr if there are no subscriptions yet (run() will retry).
    std::unique_ptr<bpt::common::ws::AnyWsStream> connect_and_subscribe() override;
    void read_loop(bpt::common::ws::AnyWsStream& ws) override;
    void parse_frame(std::string_view payload, uint64_t recv_ns) override;

private:
    void run_funding_rate_loop();

    BinanceParser parser_;

    // Funding-rate thread state — separate from the main IO thread.
    boost::asio::io_context fr_ioc_;
    boost::asio::ssl::context fr_ssl_ctx_;
    std::thread fr_thread_;

    // Reused simdjson parser + padded buffer for the funding-rate thread.
    simdjson::ondemand::parser fr_json_parser_;
    std::vector<char> fr_padded_buf_;
    std::string lower_sym_;  // scratch buffer for symbol lowercase conversion
};

}  // namespace bpt::md_gateway::adapter
