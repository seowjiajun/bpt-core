#pragma once

#include "features/ewma.h"
#include "features/fair_value.h"
#include "strategy/config/config.h"
#include "strategy/md/i_md_client.h"
#include "strategy/order/order_manager.h"
#include "strategy/refdata/i_refdata_client.h"
#include "strategy/refdata/instrument.h"
#include "strategy/strategy/i_strategy.h"
#include "strategy/strategy/position_tracker.h"
#include "strategy/unwind/graceful_unwinder.h"

#include <messages/AccountSnapshot.h>
#include <messages/ExchangeId.h>
#include <messages/ExecutionReport.h>
#include <messages/MdMarketData.h>
#include <messages/MdTrade.h>
#include <messages/OrderSide.h>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bpt::strategy::strategy {

using bpt::features::EwmaVariance;
using bpt::features::FairValueEstimator;

class FairValueMmStrategy : public IStrategy {
public:
    FairValueMmStrategy(uint64_t correlation_id,
                        const config::StrategyConfig& cfg,
                        refdata::IRefdataClient& refdata,
                        md::IMdClient* md,
                        order::OrderManager* order_mgr);

    void start() override;
    void on_snapshot(const refdata::InstrumentCache& cache) override;
    void on_delta(const refdata::Instrument& inst, bpt::messages::DeltaUpdateType::Value update_type) override;
    void on_bbo(const bpt::messages::MdMarketData& tick) override;
    void on_trade(const bpt::messages::MdTrade&) override {}
    void on_exec_report(const bpt::messages::ExecutionReport& rpt) override;
    void on_refdata_stale_changed(bool stale) override;
    std::size_t on_account_snapshot(bpt::messages::AccountSnapshot& snap) override;
    std::string get_strategy_state_json() override;
    void on_shutdown_flatten() override;
    [[nodiscard]] bool has_pending_flatten() const override;
    void on_flatten_tick() override;
    [[nodiscard]] double shutdown_drain_budget_s() const override;

private:
    static constexpr uint8_t kTagQuote = 0;
    static constexpr uint8_t kTagUnwindNormal = 1;

    struct InstrumentState {
        uint64_t instrument_id{0};
        EwmaVariance ewma_var;
        double last_mid{0.0};
        uint64_t last_tick_ns{0};
        double last_market_bid{0.0};
        double last_market_ask{0.0};
        double last_bid_price{0.0};
        double last_ask_price{0.0};
        double bid_placed_mid{0.0};
        double ask_placed_mid{0.0};
        order::OrderHandle h_bid;
        order::OrderHandle h_ask;
        order::OrderHandle h_unwind;  // mid-session inventory unwind only (kTagUnwindNormal)
        uint32_t consecutive_exchange_errors{0};
        uint64_t reject_backoff_until_ns{0};
        uint64_t pause_until_ns{0};
        double tick_size{0.0};
        double lot_size{0.0};
        std::string symbol;
        std::string exchange;
        bpt::messages::ExchangeId::Value exchange_id{bpt::messages::ExchangeId::NULL_VALUE};
        refdata::InstrumentType instrument_type{refdata::InstrumentType::SPOT};
        FairValueEstimator fv;
    };

    struct QuoteTarget {
        double bid{0.0};
        double ask{0.0};
        double half_spread_bps{0.0};
    };

    [[nodiscard]] std::optional<QuoteTarget> compute_quotes(const InstrumentState& st, double net_qty) const;
    void maybe_requote(InstrumentState& st, double net_qty, const QuoteTarget& q, uint64_t ts_ns);
    [[nodiscard]] order::OrderHandle send_limit_order(InstrumentState& st,
                                                      bpt::messages::OrderSide::Value side,
                                                      double price,
                                                      double qty);
    [[nodiscard]] order::OrderHandle send_unwind_order(InstrumentState& st,
                                                       bpt::messages::OrderSide::Value side,
                                                       double mid,
                                                       double qty,
                                                       uint8_t tag);
    [[nodiscard]] double effective_order_qty(const InstrumentState& st) const;
    [[nodiscard]] double effective_max_inventory(const InstrumentState& st) const;

    uint64_t correlation_id_;
    double vol_halflife_s_;
    std::size_t vol_warmup_ticks_;
    double spread_vol_mult_;
    double min_spread_bps_;
    double max_spread_bps_;
    double skew_alpha_;
    double one_sided_threshold_;
    double requote_threshold_;
    double max_inventory_;
    double order_qty_;
    double order_qty_fraction_;
    double order_qty_min_;
    double max_inventory_fraction_;
    double pause_below_rpnl_usd_;
    double pause_cooldown_s_;
    unwind::GracefulUnwinder unwinder_;
    FairValueEstimator::Config fv_cfg_;

    std::vector<std::string> instruments_;
    std::vector<std::string> md_exchanges_;
    std::unordered_map<std::string, config::VenueExecConfig> venue_exec_;

    refdata::IRefdataClient& refdata_;
    md::IMdClient* md_client_;
    order::OrderManager* order_mgr_;
    std::unordered_map<uint64_t, InstrumentState> state_;
    PositionTracker positions_;

    // Symbol → snapshot qty (authoritative exchange view for shutdown flatten).
    std::unordered_map<std::string, int64_t> last_snapshot_qty_e8_;
    uint64_t last_snapshot_ns_{0};
    int64_t last_equity_e8_{0};
    bool refdata_stale_{false};
    bool shutting_down_{false};
};

}  // namespace bpt::strategy::strategy
