#pragma once

/// \file
/// \brief BacktesterApp — orchestrates the backtest run loop and venue mocks.

#include "backtester/clock/clock_master.h"
#include "backtester/config/settings.h"
#include "backtester/data/data_loader.h"
#include "backtester/exchange/binance/binance_md_server.h"
#include "backtester/exchange/binance/binance_order_server.h"
#include "backtester/exchange/hyperliquid/hyperliquid_info_server.h"
#include "backtester/exchange/hyperliquid/hyperliquid_md_server.h"
#include "backtester/exchange/hyperliquid/hyperliquid_order_server.h"
#include "backtester/exchange/okx/okx_md_server.h"
#include "backtester/exchange/okx/okx_order_server.h"
#include "backtester/latency/latency_model.h"
#include "backtester/matching/matching_engine.h"
#include "backtester/messaging/aeron_bus.h"
#include "backtester/results/results_collector.h"

#include <bpt_app/app.h>
#include <memory>

namespace bpt::backtester {

class BacktesterApp : public bpt::app::IService {
public:
    BacktesterApp(config::Settings settings, messaging::BacktesterBus bus);

    /// \brief Blocking run loop — feeds ticks until the backtest window is exhausted or a signal fires.
    void run() override;

private:
    /// \brief Routes a fill back to (a) the ResultsCollector and (b) the venue-specific
    ///        OrderServer so its WS clients (the live order-gateway in backtest mode) see
    ///        the matching exec report.
    ///
    /// Unknown venues are logged + dropped.
    void on_fill(const matching::FillReport& fill);

    config::Settings settings_;
    messaging::BacktesterBus bus_;

    std::unique_ptr<data::DataLoader> loader_;
    std::unique_ptr<exchange::BinanceMdServer> binance_md_server_;
    std::unique_ptr<exchange::OkxMdServer> okx_md_server_;
    std::unique_ptr<exchange::HyperliquidMdServer> hyperliquid_md_server_;
    std::unique_ptr<exchange::HyperliquidInfoServer> hyperliquid_info_server_;
    /// latency_model_ must outlive matching_engine_ — the engine holds a
    /// non-owning pointer (set via set_latency_model). Declared first so
    /// destruction proceeds in reverse order. Concrete type so we can call
    /// set_spec/set_default during init.
    std::unique_ptr<latency::ParametricLatencyModel> latency_model_;
    std::unique_ptr<matching::MatchingEngine> matching_engine_;
    std::unique_ptr<exchange::BinanceOrderServer> binance_order_server_;
    std::unique_ptr<exchange::OkxOrderServer> okx_order_server_;
    std::unique_ptr<exchange::HyperliquidOrderServer> hyperliquid_order_server_;
    std::unique_ptr<results::ResultsCollector> results_;
    std::unique_ptr<clock::ClockMaster> clock_master_;
};

}  // namespace bpt::backtester
