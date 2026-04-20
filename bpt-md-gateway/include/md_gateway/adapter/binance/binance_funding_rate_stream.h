#pragma once

// Binance funding-rate WebSocket stream.
//
// Connects to fstream.binance.com/stream?streams=!markPrice@arr@1s —
// a separate endpoint from the main MD stream, so it gets its own
// io_context, SSL context, and thread. Parses the markPrice array and
// emits a FundingRateUpdate per subscribed instrument via the callback.
//
// Owned by BinanceAdapter; start/stop are driven by the adapter's
// lifecycle. Observes the adapter's stop_flag for shutdown.

#include "md_gateway/adapter/common/subscription_map.h"
#include "md_gateway/config/settings.h"
#include "md_gateway/messaging/funding_rate_publisher.h"

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <simdjson.h>
#include <string>
#include <thread>
#include <vector>

namespace bpt::md_gateway::adapter {

class BinanceFundingRateStream {
public:
    BinanceFundingRateStream(const config::AdapterConfig& cfg,
                             const SubscriptionMap& subs,
                             messaging::FundingRateCallback& on_funding_rate,
                             std::atomic<bool>& stop_flag);

    BinanceFundingRateStream(const BinanceFundingRateStream&) = delete;
    BinanceFundingRateStream& operator=(const BinanceFundingRateStream&) = delete;

    // Launches the IO thread. Idempotent-safe — the parent adapter
    // gates start() behind its own lifecycle.
    void start();

    // Stops the io_context (to unblock an in-flight ws.read) and joins
    // the thread. Safe to call after the adapter's stop_flag has
    // already been set.
    void stop();

private:
    void run();

    const config::AdapterConfig& cfg_;
    const SubscriptionMap& subs_;
    messaging::FundingRateCallback& on_funding_rate_;
    std::atomic<bool>& stop_flag_;

    boost::asio::io_context ioc_;
    boost::asio::ssl::context ssl_ctx_;
    std::thread thread_;

    // Reused simdjson parser + padded buffer — avoid per-message
    // allocations on the ~1 Hz markPrice@arr feed.
    simdjson::ondemand::parser json_parser_;
    std::vector<char> padded_buf_;
    std::string lower_sym_;
};

}  // namespace bpt::md_gateway::adapter
