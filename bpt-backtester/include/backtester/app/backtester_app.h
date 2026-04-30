#pragma once

#include "backtester/clock/clock_master.h"
#include "backtester/config/settings.h"
#include "backtester/data/data_loader.h"
#include "backtester/exchange/binance_md_server.h"
#include "backtester/exchange/binance_order_server.h"
#include "backtester/exchange/hyperliquid_info_server.h"
#include "backtester/exchange/hyperliquid_md_server.h"
#include "backtester/exchange/hyperliquid_order_server.h"
#include "backtester/exchange/okx_md_server.h"
#include "backtester/exchange/okx_order_server.h"
#include "backtester/matching/matching_engine.h"
#include "backtester/messaging/aeron_bus.h"
#include "backtester/results/results_collector.h"

#include <memory>
#include <bpt_app/app.h>

namespace bpt::backtester {

class BacktesterApp : public bpt::app::IService {
public:
    BacktesterApp(config::Settings settings, messaging::BacktesterBus bus);

    // Blocking run loop — feeds ticks until the backtest window is exhausted or a signal fires.
    void run() override;

private:
    config::Settings settings_;
    messaging::BacktesterBus bus_;

    std::unique_ptr<data::DataLoader> loader_;
    std::unique_ptr<exchange::BinanceMdServer> binance_md_server_;
    std::unique_ptr<exchange::OkxMdServer> okx_md_server_;
    std::unique_ptr<exchange::HyperliquidMdServer> hyperliquid_md_server_;
    std::unique_ptr<exchange::HyperliquidInfoServer> hyperliquid_info_server_;
    std::unique_ptr<matching::MatchingEngine> matching_engine_;
    std::unique_ptr<exchange::BinanceOrderServer> binance_order_server_;
    std::unique_ptr<exchange::OkxOrderServer> okx_order_server_;
    std::unique_ptr<exchange::HyperliquidOrderServer> hyperliquid_order_server_;
    std::unique_ptr<results::ResultsCollector> results_;
    std::unique_ptr<clock::ClockMaster> clock_master_;
};

}  // namespace bpt::backtester
