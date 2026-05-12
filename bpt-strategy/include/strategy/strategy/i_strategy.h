#pragma once

#include "strategy/refdata/instrument_cache.h"

#include <messages/AccountSnapshot.h>
#include <messages/DeltaUpdateType.h>
#include <messages/ExecutionReport.h>
#include <messages/MdMarketData.h>
#include <messages/MdOrderBook.h>
#include <messages/MdTrade.h>
#include <messages/VolSurface.h>

#include <analytics/messaging/toxicity_update.h>
#include <cstdint>
#include <string>
#include <vector>

namespace bpt::strategy::strategy {

// Snapshot of current portfolio state — queried by StrategyApp to publish
// to the dashboard bridge.  Options strategies populate this; linear
// strategies return the default (empty legs, zero Greeks).
struct PortfolioState {
    struct Leg {
        uint64_t instrument_id{0};
        std::string symbol;
        std::string underlying;
        uint32_t expiry_date{0};  // YYYYMMDD, 0 for perps
        double strike{0.0};
        bool is_call{true};
        bool is_option{true};  // false for perp hedge legs
        double qty{0.0};       // +ve = long, -ve = short
        double entry_price{0.0};
        double mark_price{0.0};  // current mid
        double iv{0.0};
        double delta{0.0};
        double gamma{0.0};
        double vega{0.0};
        double theta{0.0};
        double unrealized_pnl{0.0};
    };

    struct SurfacePoint {
        uint64_t instrument_id{0};
        uint32_t expiry_date{0};
        double strike{0.0};
        bool is_call{true};
        double iv{0.0};
        double bid_iv{0.0};
        double ask_iv{0.0};
        double delta{0.0};
        double time_to_expiry{0.0};
    };

    std::vector<Leg> legs;
    std::vector<SurfacePoint> surface_points;

    double portfolio_delta{0.0};
    double portfolio_gamma{0.0};
    double portfolio_vega{0.0};
    double portfolio_theta{0.0};
    double total_unrealized_pnl{0.0};
    double total_realized_pnl{0.0};

    uint64_t timestamp_ns{0};
};

class IStrategy {
public:
    virtual ~IStrategy() = default;

    // Activates the strategy
    virtual void start() = 0;

    // Callbacks from RefdataClient
    virtual void on_snapshot(const refdata::InstrumentCache& cache) = 0;
    virtual void on_delta(const refdata::Instrument& inst, bpt::messages::DeltaUpdateType::Value update_type) = 0;

    // Callbacks from MdClient
    virtual void on_bbo(const bpt::messages::MdMarketData& tick) = 0;
    virtual void on_trade(const bpt::messages::MdTrade& tick) = 0;

    // Fired when MdGateway is configured with order_book_depth > 0.
    // Default no-op — only market-making strategies need to override this.
    virtual void on_order_book(const bpt::messages::MdOrderBook& /*book*/) {}

    // Fired when Pricer publishes a new vol surface snapshot.
    // Non-const ref: SBE group iterators are stateful and require mutation.
    // Default no-op — only options strategies need to override this.
    virtual void on_vol_surface(bpt::messages::VolSurface& /*surface*/) {}

    // Fired for every execution report from OrderGateway.
    // Default no-op — strategies that manage positions must override this.
    virtual void on_exec_report(const bpt::messages::ExecutionReport& /*rpt*/) {}

    // Fired when Analytics publishes a new toxicity score update.
    // Default no-op — only strategies that want live toxicity feedback override this.
    virtual void on_toxicity_update(const bpt::analytics::messaging::ToxicityUpdate& /*update*/) {}

    // Fired by StrategyApp when the refdata heartbeat staleness state
    // changes. true → refdata heartbeat aged past the configured
    // threshold, strategy should stop emitting NEW quotes (cancels of
    // existing orders should still flow). false → fresh heartbeat
    // received, strategy may resume normal quoting.
    //
    // Default no-op — strategies that read fee_cache / funding_rate_cache
    // on the hot path (presently AS) should override this. Other
    // strategies survive refdata loss because they only consume snapshot
    // data, which is cached in InstrumentState at on_snapshot() time.
    virtual void on_refdata_stale_changed(bool /*stale*/) {}

    // Fired once per exchange at startup when the account snapshot is received.
    // Non-const ref: SBE group iterators are stateful.
    // Default no-op — strategies that need startup position seeding should override this.
    //
    // Returns the count of reconciliation divergences observed against
    // this snapshot (0 if the strategy doesn't reconcile or found none).
    // StrategyApp mirrors this into the strategy_reconciliation_divergences_total
    // Prometheus counter for alerting. A non-zero return = silent fill lost, scale
    // bug, or exchange state drift — worth waking someone up.
    virtual std::size_t on_account_snapshot(bpt::messages::AccountSnapshot& /*snap*/) { return 0; }

    // Returns the current portfolio state for dashboard publishing.
    // Default returns empty — only options strategies with position
    // tracking need to override this.
    virtual PortfolioState get_portfolio_state() { return {}; }

    // Strategy state snapshot for the dashboard strategy-state panel.
    // Published as JSON on the dashboard snapshot stream alongside
    // portfolio state. Default returns empty string (no state to publish).
    //
    // Schema convention:
    //   {
    //     "type": "strategyState",   // routed by dashboard ws/client.ts
    //     "kind": "AS",              // discriminator → frontend picks the
    //                                // matching panel from panels/index.ts
    //                                // (one entry per strategy class)
    //     ...strategy-specific fields
    //   }
    // The dashboard's GenericStrategyPanel renders unknown `kind`s as a
    // JSON dump so adding a new strategy is non-fatal — operators still
    // see something while the dedicated panel is being built.
    virtual std::string get_strategy_state_json() { return {}; }

    // Called by StrategyApp on graceful shutdown (SIGTERM/SIGINT) after
    // the main poll loop exits. Implementations should cancel any
    // resting orders and fire offsetting market IOCs to flatten every
    // non-zero position. This runs on the main thread with the order
    // path still wired up, so fires go through the normal OrderManager
    // → OrderGateway → exchange path. StrategyApp drains exec reports for a
    // short window after this returns so fills have a chance to be
    // acked before the process exits.
    //
    // Default no-op — only strategies that hold inventory or post
    // resting quotes need to override this. Crash-safety (kill -9, OOM,
    // host reboot) is NOT covered here; that needs an exchange-side
    // dead-man switch in the order-gateway HL adapter.
    virtual void on_shutdown_flatten() {}

    // True if the strategy has resting orders or in-flight unwind
    // orders that haven't reached a terminal state yet. StrategyApp's
    // shutdown drain loops on this to exit early once everything is
    // clean, instead of blindly sleep-spinning for the full timeout
    // budget. A strategy that doesn't override it returns false —
    // appropriate for stateless / non-inventory strategies.
    [[nodiscard]] virtual bool has_pending_flatten() const { return false; }

    // Persist warm-start state (EWMA estimators, regime detector, etc.)
    // to `path`. StrategyApp calls this on graceful shutdown AFTER the
    // flatten drain completes, so positions are flat and state is
    // stable. Default no-op — strategies with no expensive warmup don't
    // need to persist anything.
    //
    // Implementations should write atomically (e.g. tmp + rename) to
    // avoid leaving a half-written file behind on crash; returning
    // normally signals "saved OK" and any error should be logged
    // internally rather than thrown.
    virtual void save_state(const std::string& /*path*/) {}

    // Load warm-start state from `path` if it exists and is no older
    // than `max_age_s` seconds. Called once, AFTER on_snapshot() has
    // resolved the instrument universe (so instrument_id → state
    // entries can be matched). Missing / corrupt / stale file is not
    // an error — strategies fall back to cold start silently.
    virtual void load_state(const std::string& /*path*/, uint64_t /*max_age_s*/) {}
};

}  // namespace bpt::strategy::strategy
