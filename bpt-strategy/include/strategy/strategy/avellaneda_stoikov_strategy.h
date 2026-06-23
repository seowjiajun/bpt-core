#pragma once

#include "bpt_common/book/order_book_state.h"
#include "features/ewma.h"
#include "features/fair_value.h"
#include "features/ofi.h"
#include "features/queue.h"
#include "features/regime_detector.h"
#include "features/vol_gate.h"
#include "strategy/config/config.h"
#include "strategy/config/sizer.h"
#include "strategy/md/md_client.h"
#include "strategy/order/order_manager.h"
#include "strategy/refdata/refdata_client.h"
#include "strategy/strategy/as_pricer.h"
#include "strategy/strategy/canonical_resolver.h"
#include "strategy/strategy/i_strategy.h"
#include "strategy/strategy/position_tracker.h"
#include "strategy/strategy/suppression_policy.h"
#include "strategy/unwind/graceful_unwinder.h"

#include <messages/ExchangeId.h>
#include <messages/ExecutionReport.h>
#include <messages/MdMarketData.h>
#include <messages/MdOrderBook.h>
#include <messages/MdTrade.h>
#include <messages/OrderSide.h>

#include <atomic>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bpt::strategy::strategy {

using bpt::common::book::OrderBookState;
using bpt::features::EwmaDrift;
using bpt::features::EwmaVariance;
using bpt::features::FairValueEstimator;
using bpt::features::KappaEstimator;
using bpt::features::OFICalculator;
using bpt::features::QueueTracker;
using bpt::features::RegimeDetector;
using bpt::features::TimeWeightedEwma;
using bpt::features::VolatilityGate;

class AvellanedaStoikovStrategy : public IStrategy {
public:
    AvellanedaStoikovStrategy(uint64_t correlation_id,
                              const config::StrategyConfig& cfg,
                              refdata::IRefdataClient& refdata,
                              md::IMdClient* md,
                              order::OrderManager* order_mgr);

    void start() override;
    void on_instrument_snapshot(const refdata::InstrumentCache& cache) override;
    void on_instrument_delta(const refdata::Instrument& inst, bpt::messages::DeltaUpdateType::Value update_type) override;
    void on_bbo(const bpt::messages::MdMarketData& tick) override;
    void on_trade(const bpt::messages::MdTrade& tick) override;
    void on_order_book(const bpt::messages::MdOrderBook& book) override;
    void on_exec_report(const bpt::messages::ExecutionReport& rpt) override;
    std::size_t on_account_snapshot(bpt::messages::AccountSnapshot& snap) override;
    void on_toxicity_update(const bpt::analytics::messaging::ToxicityUpdate& update) override;
    void on_refdata_stale_changed(bool stale) override;
    std::string get_strategy_state_json() override;
    void on_shutdown_flatten() override;
    [[nodiscard]] bool has_pending_flatten() const override;
    void on_flatten_tick() override;
    [[nodiscard]] double shutdown_drain_budget_s() const override;

    void save_state(const std::string& path) override;
    void load_state(const std::string& path, uint64_t max_age_s) override;

private:
    static constexpr uint8_t kTagQuote = 0;
    static constexpr uint8_t kTagUnwindNormal = 1;

    struct InstrumentState {
        // ── Metadata ────────────────────────────────────────────────────────────
        uint64_t instrument_id{0};
        std::string symbol;
        std::string exchange;
        bpt::messages::ExchangeId::Value exchange_id{bpt::messages::ExchangeId::NULL_VALUE};
        refdata::InstrumentType instrument_type{refdata::InstrumentType::SPOT};
        std::string base_ccy;
        double tick_size{0.0};
        double lot_size{0.0};

        // ── Vol / drift / regime ─────────────────────────────────────────────
        EwmaVariance ewma_var;
        EwmaDrift ewma_drift;
        KappaEstimator ewma_kappa;
        RegimeDetector regime;
        double last_mid{0.0};
        uint64_t last_tick_ns{0};
        uint64_t last_trade_ns{0};
        uint64_t session_start_ns{0};
        double slow_drift_bps{0.0};
        double slow_drift_window_start_mid{0.0};
        uint64_t slow_drift_window_start_ns{0};

        // ── Resting quotes ────────────────────────────────────────────────────
        order::OrderHandle h_bid;
        order::OrderHandle h_ask;
        order::OrderHandle h_unwind;
        double last_bid_price{0.0};
        double last_ask_price{0.0};
        double last_market_bid{0.0};
        double last_market_ask{0.0};
        double bid_placed_mid{0.0};
        double ask_placed_mid{0.0};

        // ── Risk / suppression inputs ─────────────────────────────────────────
        VolatilityGate vol_gate;
        double tox_bid_toxicity{0.0};
        double tox_ask_toxicity{0.0};
        bool tox_data_received{false};
        uint32_t consecutive_exchange_errors{0};
        uint64_t reject_backoff_until_ns{0};
        uint64_t pause_until_ns{0};

        // ── Fill tracking / post-fill markout ─────────────────────────────────
        double pending_buy_fill_price{0.0};
        uint64_t pending_buy_fill_ts{0};
        double pending_sell_fill_price{0.0};
        uint64_t pending_sell_fill_ts{0};
        uint64_t post_fill_suspend_until_bid{0};
        uint64_t post_fill_suspend_until_ask{0};
        std::deque<double> recent_rpnl;

        // ── Order book / queue / fair value / OFI ─────────────────────────────
        OrderBookState book;
        QueueTracker queue;
        FairValueEstimator fv;
        OFICalculator ofi{OFICalculator::Config{}};
        TimeWeightedEwma ewma_ofi_sq{60.0};
        uint64_t ewma_ofi_last_ns{0};
        std::vector<OrderBookState::Level> ofi_top_bid_buf;
        std::vector<OrderBookState::Level> ofi_top_ask_buf;
        std::vector<OFICalculator::Level> ofi_bids_buf;
        std::vector<OFICalculator::Level> ofi_asks_buf;
    };

    [[nodiscard]] InstrumentState make_instrument_state(uint64_t instrument_id,
                                                        const std::string& symbol,
                                                        const std::string& exchange,
                                                        bpt::messages::ExchangeId::Value exchange_id,
                                                        refdata::InstrumentType type,
                                                        const std::string& base_ccy,
                                                        double tick_size,
                                                        double lot_size) const;

    [[nodiscard]] InstrumentState* find_state(uint64_t instrument_id) {
        const auto it = state_.find(instrument_id);
        return it != state_.end() ? &it->second : nullptr;
    }

    void maybe_requote(InstrumentState& st, const BboContext& ctx, const QuoteTarget& quotes);
    order::OrderHandle send_unwind_order(InstrumentState& st, bpt::messages::OrderSide::Value side,
                                         double mid, double qty, uint8_t tag);
    order::OrderHandle send_limit_order(InstrumentState& st, bpt::messages::OrderSide::Value side,
                                        double price, double qty);

    uint64_t correlation_id_;
    double vol_halflife_s_;
    double kappa_halflife_s_;
    double drift_halflife_s_;
    double requote_threshold_;
    uint8_t order_book_depth_;
    FairValueEstimator::Config fv_cfg_;
    double pause_below_rpnl_usd_;
    double pause_cooldown_s_;
    double post_fill_markout_threshold_bps_;
    double post_fill_markout_cooldown_s_;
    double slow_drift_window_s_;
    SuppressionPolicy supp_policy_;
    ASPricer pricer_;
    unwind::GracefulUnwinder unwinder_;
    RegimeDetector::Config regime_cfg_;
    VolatilityGate::Config vol_gate_cfg_;
    double vol_gate_sigma_mult_;
    uint64_t ofi_window_ns_;
    double ofi_sigma_halflife_s_;
    config::Sizer sizer_;

    std::vector<std::string> instruments_;
    std::vector<std::string> md_exchanges_;
    std::unordered_map<bpt::messages::ExchangeId::Value, config::VenueExecConfig> venue_exec_;

    refdata::IRefdataClient& refdata_;
    md::IMdClient* md_client_;
    order::OrderManager* order_mgr_;
    std::unordered_map<uint64_t, InstrumentState> state_;
    PositionTracker positions_;

    struct SnapshotKey {
        bpt::messages::ExchangeId::Value exchange_id;
        std::string symbol;
        bool operator==(const SnapshotKey& o) const noexcept {
            return exchange_id == o.exchange_id && symbol == o.symbol;
        }
    };
    struct SnapshotKeyHash {
        std::size_t operator()(const SnapshotKey& k) const noexcept {
            return std::hash<std::string>{}(k.symbol) ^
                   (static_cast<std::size_t>(k.exchange_id) * 0x9E3779B97F4A7C15ULL);
        }
    };
    std::unordered_map<SnapshotKey, int64_t, SnapshotKeyHash> last_snapshot_qty_e8_;
    uint64_t last_snapshot_ns_{0};

    struct CcyKey {
        bpt::messages::ExchangeId::Value exchange_id;
        std::string ccy;
        bool operator==(const CcyKey& o) const noexcept { return exchange_id == o.exchange_id && ccy == o.ccy; }
    };
    struct CcyKeyHash {
        std::size_t operator()(const CcyKey& k) const noexcept {
            return std::hash<std::string>{}(k.ccy) ^ (static_cast<std::size_t>(k.exchange_id) * 0x9E3779B97F4A7C15ULL);
        }
    };
    std::unordered_map<CcyKey, int64_t, CcyKeyHash> initial_ccy_equity_e8_;
    bool initial_ccy_equity_captured_{false};
    int64_t last_equity_e8_{0};
    bool refdata_stale_{false};
};

}  // namespace bpt::strategy::strategy
