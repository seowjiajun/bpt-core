#pragma once

#include "backtester/clock/clock_master.h"
#include "backtester/config/settings.h"
#include "backtester/data/data_loader.h"
#include "backtester/exchange/binance_md_server.h"
#include "backtester/exchange/binance_order_server.h"
#include "backtester/exchange/okx_md_server.h"
#include "backtester/exchange/okx_order_server.h"
#include "backtester/matching/matching_engine.h"
#include "backtester/messaging/backtest_ack_subscriber.h"
#include "backtester/messaging/backtest_control_publisher.h"
#include "backtester/results/results_collector.h"

#include <Aeron.h>

#include <memory>

namespace bpt::backtester {

class BacktesterApp {
public:
    explicit BacktesterApp(config::Settings settings);

    // Blocking run loop — feeds ticks until the backtest window is exhausted or a signal fires.
    void run();

private:
    config::Settings settings_;

    // Aeron (tick-gating channel to Strategy)
    std::shared_ptr<aeron::Aeron> aeron_;
    std::unique_ptr<messaging::BacktestControlPublisher> ctrl_pub_;
    std::unique_ptr<messaging::BacktestAckSubscriber> ack_sub_;

    std::unique_ptr<data::DataLoader> loader_;
    std::unique_ptr<exchange::BinanceMdServer> binance_md_server_;
    std::unique_ptr<exchange::OkxMdServer> okx_md_server_;
    std::unique_ptr<matching::MatchingEngine> matching_engine_;
    std::unique_ptr<exchange::BinanceOrderServer> binance_order_server_;
    std::unique_ptr<exchange::OkxOrderServer> okx_order_server_;
    std::unique_ptr<results::ResultsCollector> results_;
    std::unique_ptr<clock::ClockMaster> clock_master_;
};

}  // namespace bpt::backtester
