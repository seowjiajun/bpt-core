#pragma once

#include "strategy/config/config.h"
#include "strategy/md/md_client.h"
#include "strategy/order/order_manager.h"
#include "strategy/refdata/refdata_client.h"
#include "strategy/strategy/canonical_resolver.h"
#include "strategy/strategy/hmm_filter.h"
#include "strategy/strategy/i_strategy.h"
#include "strategy/strategy/position_tracker.h"

#include <messages/ExchangeId.h>
#include <messages/ExecutionReport.h>
#include <messages/MdMarketData.h>
#include <messages/MdOrderBook.h>
#include <messages/MdTrade.h>
#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <messages/TimeInForce.h>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bpt::strategy::strategy {

// HMM-based regime-switching meta-strategy.
//
// Maintains a per-instrument Hidden Markov Model that infers the current
// market regime from a 5-feature observation vector built from 1s bars:
//
//   [log_return_1min, ewma_vol, spread_bps, book_imbalance, vol_zscore]
//
// Four regimes map to dedicated sub-strategies:
//   TRENDING_UP / TRENDING_DOWN → EMA-crossover momentum
//   MEAN_REVERT                 → VWAP-deviation mean reversion
//   HIGH_VOL                    → Avellaneda-Stoikov market making
//
// Regime switches cancel all open orders and close the existing position
// before activating the new sub-strategy.  A confidence threshold and a
// minimum dwell count prevent thrashing on regime-boundary noise.
//
// Model parameters default to crypto-perp priors (see HmmFilter::default_params).
// All sub-strategy signal parameters are configurable via strategy.params TOML.
class HmmStrategy : public IStrategy {
public:
    HmmStrategy(uint64_t correlation_id,
                const config::StrategyConfig& cfg,
                refdata::RefdataClient& refdata,
                md::MdClient* md,
                order::OrderManager* order_mgr);

    void start() override;
    void on_snapshot(const refdata::InstrumentCache& cache) override;
    void on_delta(const refdata::Instrument& inst, bpt::messages::DeltaUpdateType::Value update_type) override;
    void on_bbo(const bpt::messages::MdMarketData& tick) override;
    void on_trade(const bpt::messages::MdTrade& tick) override;
    void on_order_book(const bpt::messages::MdOrderBook& book) override;
    void on_exec_report(const bpt::messages::ExecutionReport& rpt) override;

private:
    // ── Per-instrument state ─────────────────────────────────────────────────
    struct InstrumentState {
        // Identity
        uint64_t instrument_id{0};
        std::string symbol;
        std::string exchange;
        bpt::messages::ExchangeId::Value exchange_id{bpt::messages::ExchangeId::NULL_VALUE};
        double tick_size{0.0};
        double lot_size{0.0};

        // BBO
        double bid{0.0}, ask{0.0};
        uint64_t last_bbo_ns{0};

        // 1s bar
        double bar_open{0.0}, bar_high{0.0}, bar_low{0.0}, bar_close{0.0};
        uint64_t bar_start_ns{0};
        double prev_bar_close{0.0};

        // ATR (EMA of per-bar true range)
        double bar_atr{0.0};
        int bar_atr_warmup{0};

        // ── Feature 0: 1-min rolling log return ─────────────────────────────
        static constexpr size_t kReturnWindow = 60;
        std::array<double, kReturnWindow> bar_returns{};  // ring buffer of bar log returns
        size_t return_head{0};
        size_t return_count{0};
        double return_sum{0.0};  // rolling sum = log(close_now / close_60_bars_ago)

        // ── Feature 1: EWMA variance (λ=0.94) ───────────────────────────────
        double ewma_var{0.0};  // sqrt(ewma_var) = per-bar realised vol

        // ── Feature 3: order-book imbalance ─────────────────────────────────
        double book_imbalance{0.5};  // updated by on_order_book; default neutral

        // ── Feature 4: trade-volume Z-score ─────────────────────────────────
        double bar_trade_vol{0.0};  // accumulates trade qty during current bar
        static constexpr size_t kVolWindow = 60;
        std::array<double, kVolWindow> vol_buf{};
        size_t vol_head{0};
        size_t vol_count{0};
        double vol_sum{0.0};
        double vol_sum_sq{0.0};

        // ── Rolling VWAP (reversion sub-strategy) ───────────────────────────
        double bar_vwap_num{0.0};  // sum(price × qty) for current bar
        double bar_vwap_den{0.0};  // sum(qty) for current bar
        static constexpr size_t kVwapWindow = 60;
        std::array<double, kVwapWindow> vwap_num_buf{};
        std::array<double, kVwapWindow> vwap_den_buf{};
        size_t vwap_head{0};
        size_t vwap_count{0};
        double vwap_num_sum{0.0};
        double vwap_den_sum{0.0};

        // ── HMM inference ────────────────────────────────────────────────────
        HmmFilter hmm;
        HmmFilter::State regime{HmmFilter::State::MEAN_REVERT};
        HmmFilter::State target_regime{HmmFilter::State::MEAN_REVERT};
        int regime_dwell{0};  // consecutive bar ticks confirming current dominant
        bool warming_up{true};

        // ── Regime transition ────────────────────────────────────────────────
        bool transitioning{false};
        bool closing_position{false};
        std::unordered_set<uint64_t> pending_cancels;
        uint64_t close_order_id{0};
        uint64_t transition_start_ns{0};

        // ── Momentum sub-strategy ────────────────────────────────────────────
        double ema_fast{0.0}, ema_slow{0.0};
        int ema_warmup{0};
        bool has_momentum_position{false};
        double momentum_entry{0.0}, momentum_stop{0.0}, momentum_target{0.0};
        bpt::messages::OrderSide::Value momentum_side{bpt::messages::OrderSide::BUY};
        uint64_t momentum_order_id{0};

        // ── VWAP reversion sub-strategy ──────────────────────────────────────
        bool has_reversion_position{false};
        bpt::messages::OrderSide::Value reversion_side{bpt::messages::OrderSide::BUY};
        double reversion_stop{0.0};
        uint64_t reversion_order_id{0};

        // ── Market making sub-strategy (Avellaneda-Stoikov) ──────────────────
        uint64_t mm_bid_id{0}, mm_ask_id{0};
        double mm_bid_price{0.0}, mm_ask_price{0.0};
        uint64_t mm_last_quote_ns{0};

        // ── Rejection backoff ────────────────────────────────────────────────
        int consecutive_rejects{0};
        uint64_t reject_cooldown_until_ns{0};
    };

    // ── Bar close ────────────────────────────────────────────────────────────
    void on_bar_close(InstrumentState& st);

    // ── HMM + regime dispatch ────────────────────────────────────────────────
    void evaluate_regime(InstrumentState& st);
    void begin_transition(InstrumentState& st, HmmFilter::State target);
    void check_transition_complete(InstrumentState& st);

    // ── Momentum sub-strategy ────────────────────────────────────────────────
    void momentum_update_ema(InstrumentState& st, double close);
    void momentum_check_signal(InstrumentState& st);
    void momentum_check_exit(InstrumentState& st, double mid);
    void momentum_close_position(InstrumentState& st);

    // ── VWAP reversion sub-strategy ──────────────────────────────────────────
    void reversion_check_signal(InstrumentState& st);
    void reversion_check_exit(InstrumentState& st, double mid);
    void reversion_close_position(InstrumentState& st);

    // ── Market making sub-strategy ───────────────────────────────────────────
    void mm_quote(InstrumentState& st);
    void mm_cancel_quotes(InstrumentState& st);

    // ── Order helpers ────────────────────────────────────────────────────────
    uint64_t send_order(InstrumentState& st,
                        bpt::messages::OrderSide::Value side,
                        bpt::messages::OrderType::Value type,
                        bpt::messages::TimeInForce::Value tif,
                        double price,
                        double qty);
    void cancel_order(InstrumentState& st, uint64_t order_id);

    // ── Utility ──────────────────────────────────────────────────────────────
    double rolling_vwap(const InstrumentState& st) const;
    double round_qty(const InstrumentState& st, double qty_usd, double mid) const;
    double round_price(const InstrumentState& st, double price, bool round_up) const;

    // ── Config ───────────────────────────────────────────────────────────────
    // HMM
    double confidence_threshold_;
    int min_dwell_bars_;
    double ewma_lambda_;
    uint64_t bar_interval_ns_;
    uint8_t order_book_depth_;

    // Momentum
    int ema_fast_period_;
    int ema_slow_period_;
    int atr_period_;
    double atr_stop_mult_;
    double atr_target_mult_;
    double momentum_qty_usd_;

    // VWAP reversion
    double vwap_deviation_atr_;
    double vwap_close_atr_;
    double vwap_stop_atr_;
    double reversion_qty_usd_;

    // Market making (Avellaneda-Stoikov)
    double mm_gamma_;
    double mm_k_;
    double mm_horizon_s_;
    double mm_qty_usd_;
    double mm_max_inventory_usd_;
    uint64_t mm_requote_ns_;
    double mm_min_spread_bps_;

    // Execution
    double aggress_bps_;
    double max_spread_bps_;

    uint64_t correlation_id_;
    std::vector<std::string> instruments_;
    std::vector<std::string> md_exchanges_;
    std::unordered_map<std::string, config::VenueExecConfig> venue_exec_;

    refdata::RefdataClient& refdata_;
    md::MdClient* md_client_;
    order::OrderManager* order_mgr_;

    std::unordered_map<uint64_t, InstrumentState> state_;
    std::unordered_map<uint64_t, uint64_t> order_to_instrument_;
    PositionTracker positions_;
};

}  // namespace bpt::strategy::strategy
