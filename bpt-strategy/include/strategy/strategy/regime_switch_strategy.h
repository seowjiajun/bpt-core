#pragma once

#include "strategy/config/config.h"
#include "strategy/md/md_client.h"
#include "strategy/order/order_manager.h"
#include "strategy/refdata/refdata_client.h"
#include "strategy/strategy/canonical_resolver.h"
#include "strategy/strategy/i_strategy.h"
#include "strategy/strategy/position_tracker.h"

#include <messages/ExchangeId.h>
#include <messages/ExecutionReport.h>
#include <messages/MdMarketData.h>
#include <messages/MdTrade.h>
#include <messages/OrderSide.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bpt::strategy::strategy {

// Regime-switching meta-strategy.
//
// Computes a rolling Hurst exponent to detect the current market regime:
//   H < 0.45 → mean-reverting → Grid trading (ladder of limit orders)
//   H > 0.55 → trending       → Momentum (EMA crossover with ATR stops)
//   0.45–0.55 → neutral       → reduce exposure, wait
//
// Transitions between regimes cancel existing orders and flatten before
// entering the new regime's logic.
class RegimeSwitchStrategy : public IStrategy {
public:
    RegimeSwitchStrategy(uint64_t correlation_id,
                         const config::StrategyConfig& cfg,
                         refdata::RefdataClient& refdata,
                         md::MdClient* md,
                         order::OrderManager* order_mgr);

    void start() override;
    void on_snapshot(const refdata::InstrumentCache& cache) override;
    void on_delta(const refdata::Instrument& inst, bpt::messages::DeltaUpdateType::Value update_type) override;
    void on_bbo(const bpt::messages::MdMarketData& tick) override;
    void on_trade(const bpt::messages::MdTrade& tick) override;
    void on_exec_report(const bpt::messages::ExecutionReport& rpt) override;
    void on_order_book(const bpt::messages::MdOrderBook& book) override;

private:
    enum class Regime : uint8_t {
        WARMING_UP,
        GRID,
        MOMENTUM,
        NEUTRAL,
        TRANSITIONING,
    };

    enum class TransitionPhase : uint8_t {
        NONE,
        CANCELLING_ORDERS,
        CLOSING_POSITION,
        READY,
    };

    static const char* regime_name(Regime r);

    // ── Hurst ────────────────────────────────────────────────────────────────
    static constexpr size_t kMaxHurstWindow = 500;
    static constexpr size_t kMaxTimeBars = 500;

    Regime classify_regime(double hurst, Regime current) const;

    // ── Grid sub-strategy ────────────────────────────────────────────────────
    static constexpr int kMaxGridLevels = 10;

    struct GridLevel {
        uint64_t order_id{0};
        double price{0.0};
        bpt::messages::OrderSide::Value side{bpt::messages::OrderSide::NULL_VALUE};
        bool is_take_profit{false};
    };

    struct GridState {
        double grid_center{0.0};
        std::array<GridLevel, 2 * kMaxGridLevels> levels{};
        int active_count{0};
    };

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
        double bid{0.0};
        double ask{0.0};
        uint64_t last_bbo_ns{0};

        // Time-sampled mid prices (1s bars)
        double bar_open{0.0};
        double bar_high{0.0};
        double bar_low{0.0};
        double bar_close{0.0};
        uint64_t bar_start_ns{0};
        double prev_bar_close{0.0};

        // Hurst ring buffer (fed from 1s bars, not raw ticks)
        std::array<double, kMaxHurstWindow> log_returns{};
        size_t return_count{0};
        size_t return_head{0};
        int bars_since_hurst_eval{0};
        double last_hurst{0.5};

        // Time-bar ATR (from 1s OHLC bars)
        double bar_atr{0.0};
        int bar_atr_warmup{0};

        // Regime
        Regime regime{Regime::WARMING_UP};
        Regime target_regime{Regime::WARMING_UP};
        TransitionPhase transition{TransitionPhase::NONE};

        // Grid
        GridState grid{};

        // Momentum
        double ema_fast{0.0};
        double ema_slow{0.0};
        int ema_warmup{0};
        bool has_momentum_position{false};
        double momentum_entry_price{0.0};
        double momentum_stop{0.0};
        double momentum_target{0.0};
        bpt::messages::OrderSide::Value momentum_side{bpt::messages::OrderSide::NULL_VALUE};
        uint64_t momentum_order_id{0};

        // Regime dwell — counts Hurst evaluations since entering current regime.
        int regime_dwell{0};

        // Transition tracking
        std::unordered_set<uint64_t> pending_cancels;
        uint64_t close_order_id{0};
        uint64_t transition_start_ns{0};

        // Rejection backoff — pause grid/momentum after consecutive rejects
        int consecutive_rejects{0};
        uint64_t reject_cooldown_until_ns{0};
    };

    // ── Time bar ─────────────────────────────────────────────────────────────
    void on_bar_close(InstrumentState& st);

    // ── Grid methods ────────────────────────────────────────────────────────
    void grid_build(InstrumentState& st);
    void grid_cancel_all(InstrumentState& st);
    void grid_on_fill(InstrumentState& st, uint64_t order_id, const bpt::messages::ExecutionReport& rpt);
    double get_round_trip_fee_bps(const InstrumentState& st) const;

    // ── Momentum methods ─────────────────────────────────────────────────────
    void momentum_update_indicators(InstrumentState& st, double mid);
    void momentum_check_signal(InstrumentState& st);
    void momentum_check_exit(InstrumentState& st, double mid);
    void momentum_close_position(InstrumentState& st);

    // ── Regime transitions ───────────────────────────────────────────────────
    void begin_transition(InstrumentState& st, Regime target);
    void check_transition_complete(InstrumentState& st);

    // ── Order helpers ────────────────────────────────────────────────────────
    uint64_t send_order(InstrumentState& st,
                        bpt::messages::OrderSide::Value side,
                        bpt::messages::OrderType::Value type,
                        bpt::messages::TimeInForce::Value tif,
                        double price,
                        double qty);
    void cancel_order(InstrumentState& st, uint64_t order_id);

    // ── Config ───────────────────────────────────────────────────────────────
    // Hurst
    size_t hurst_window_;
    int hurst_eval_ticks_;
    double mean_revert_threshold_;
    double trend_threshold_;
    double hysteresis_;
    int min_regime_dwell_;  // minimum Hurst evals before allowing regime change

    // Grid
    int grid_levels_count_;
    double grid_spacing_bps_;
    double grid_qty_usd_;
    double grid_max_position_usd_;
    double grid_recenter_bps_;

    // Momentum
    int ema_fast_period_;
    int ema_slow_period_;
    int atr_period_;
    double atr_stop_mult_;
    double atr_target_mult_;
    double momentum_qty_usd_;

    // Execution
    double aggress_bps_;
    double max_spread_bps_;     // skip trading when spread > this
    double bar_interval_ns_;    // time-bar duration (default 1s)
    uint8_t order_book_depth_;  // 0 = BBO only, 5 = top-5 levels

    // Standard fields
    uint64_t correlation_id_;
    std::vector<std::string> instruments_;
    std::vector<std::string> md_exchanges_;
    std::unordered_map<std::string, config::VenueExecConfig> venue_exec_;

    refdata::RefdataClient& refdata_;
    md::MdClient* md_client_;
    order::OrderManager* order_mgr_;
    std::unordered_map<uint64_t, InstrumentState> state_;         // keyed by instrument_id
    std::unordered_map<uint64_t, uint64_t> order_to_instrument_;  // order_id → instrument_id
    PositionTracker positions_;
    bool md_subscribed_{false};
};

}  // namespace bpt::strategy::strategy
