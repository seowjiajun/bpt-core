// AS graceful-exit path + account-snapshot reconcile.

#include "strategy/clock/sim_clock.h"
#include "strategy/strategy/avellaneda_stoikov_strategy.h"
#include "strategy/strategy/reconciler.h"

#include <messages/ExchangeId.h>

#include <bpt_common/logging.h>
#include <cmath>

namespace bpt::strategy::strategy {

namespace {
quill::Logger* kLog() {
    static quill::Logger* l = bpt::common::logging::get_logger("AS");
    return l;
}
}  // namespace

void AvellanedaStoikovStrategy::on_shutdown_flatten() {
    if (!order_mgr_)
        return;

    std::vector<unwind::GracefulUnwinder::Instrument> instruments;
    for (auto& [id, st] : state_) {
        order_mgr_->cancel_all(st.exchange_id, id);
        if (st.h_bid.live())
            order_mgr_->send_cancel(st.h_bid);
        if (st.h_ask.live())
            order_mgr_->send_cancel(st.h_ask);

        const double fv = st.fv.last_estimate();
        instruments.push_back({
            .instrument_id = id,
            .exchange_id = st.exchange_id,
            .tick_size = st.tick_size,
            .lot_size = st.lot_size,
            .symbol = st.symbol,
            .price_ref = (!std::isnan(fv) && fv > 0.0) ? fv : st.last_mid,
        });
    }
    unwinder_.arm(std::move(instruments));
}

void AvellanedaStoikovStrategy::on_flatten_tick() {
    unwinder_.tick();
}
bool AvellanedaStoikovStrategy::has_pending_flatten() const {
    return unwinder_.pending();
}
double AvellanedaStoikovStrategy::shutdown_drain_budget_s() const {
    return unwinder_.drain_budget_s();
}

std::size_t AvellanedaStoikovStrategy::on_account_snapshot(bpt::messages::AccountSnapshot& snap) {
    // Step 1: drain the SBE positions group exactly once into a map.
    // Cached for shutdown flatten (exchange-authoritative position
    // source) AND re-used for the reconcile pass below — SBE group
    // cursors can only be walked once per message.
    //
    // ORDER MATTERS: SBE repeating groups share a read cursor with the
    // parent message, so positions must be drained before
    // currencyBalances. Both helpers are non-const on snap.
    //
    // Rewind the cursor up front — strategy_service's log line calls
    // snap.positions().count() before handing us the message, which
    // advances the group cursor past the positions header. Without
    // rewinding, extract_exchange_positions reads the currencyBalances
    // header as if it were the positions header (silent corruption on
    // the old code path; crash-or-garbage once we also call
    // currencyBalances here).
    snap.sbeRewind();

    // Cache exchange-reported total equity for equity-fraction sizing
    // (see effective_order_qty / effective_max_inventory). Captured here
    // so every AccountSnapshot refresh updates the sizing baseline; if
    // equity moves, next tick's quote sizes follow without operator
    // intervention. Quoted in USD-equivalent for HL perp; SPOT venues
    // need conversion that is out of scope for the single-instrument
    // PERP path this is currently exercised on.
    last_equity_e8_ = snap.totalEquityE8();

    const auto exchange_id = snap.exchangeId();
    // Row-level extract preserves avg entry price alongside qty so we
    // can seed PositionTracker on a divergence (see reconciler loop
    // below). Legacy exchange_by_symbol_raw map kept for reconcile() +
    // the shutdown-flatten cache which only want qty.
    const auto exchange_row_by_symbol = extract_exchange_position_rows(snap);
    std::unordered_map<std::string, int64_t> exchange_by_symbol_raw;
    exchange_by_symbol_raw.reserve(exchange_row_by_symbol.size());
    for (const auto& [symbol, row] : exchange_row_by_symbol) {
        exchange_by_symbol_raw[symbol] = row.net_qty_e8;
    }
    const auto currency_equity_e8 = extract_exchange_currency_balances(snap);
    for (const auto& [symbol, qty_e8] : exchange_by_symbol_raw) {
        last_snapshot_qty_e8_[{exchange_id, symbol}] = qty_e8;
    }
    last_snapshot_ns_ = snap.timestampNs();

    // On the first post-refdata snapshot we capture the session-start
    // currency baseline for SPOT reconciliation. PositionTracker was
    // zeroed in on_snapshot() so "delta from this baseline" == "net
    // traded according to the exchange". Subsequent snapshots use the
    // captured baseline to compute the SPOT delta.
    if (!initial_ccy_equity_captured_) {
        for (const auto& [ccy, equity] : currency_equity_e8) {
            initial_ccy_equity_e8_[{exchange_id, ccy}] = equity;
        }
        initial_ccy_equity_captured_ = true;
        bpt::common::log::info(kLog(),
                               "SPOT reconcile baseline captured: {} ccy row(s) on exchange={}",
                               currency_equity_e8.size(),
                               bpt::messages::ExchangeId::c_str(exchange_id));
    }

    // Step 2: build the map we'll hand to reconcile(). For PERP/FUTURE
    // the exchange view is positions[symbol]. For SPOT that row is
    // missing (or spuriously populated by quote-currency holdings), so
    // we override with delta = current_ccy_equity - initial_ccy_equity.
    std::unordered_map<std::string, int64_t> exchange_by_symbol = exchange_by_symbol_raw;

    // Step 3: build id→symbol map for our tracker-side entries on this
    // exchange. At the same time, rewrite SPOT entries to use the
    // delta-based exchange view computed from currency balances.
    std::unordered_map<uint64_t, std::string> symbol_map;
    symbol_map.reserve(state_.size());
    for (const auto& [id, st] : state_) {
        if (st.exchange_id != exchange_id)
            continue;
        symbol_map[id] = st.symbol;

        if (st.instrument_type == refdata::InstrumentType::SPOT && !st.base_ccy.empty()) {
            const auto it_cur = currency_equity_e8.find(st.base_ccy);
            const auto it_base = initial_ccy_equity_e8_.find({exchange_id, st.base_ccy});
            if (it_cur != currency_equity_e8.end() && it_base != initial_ccy_equity_e8_.end()) {
                exchange_by_symbol[st.symbol] = it_cur->second - it_base->second;
            } else {
                // Missing baseline or missing current row → we can't
                // compute a meaningful delta. Drop the symbol so the
                // reconciler treats it as "exchange didn't report" and
                // compares against 0 — which will fire iff tracker has
                // moved from 0. Acceptable: we only enter this branch
                // on the first snapshot before baseline capture, or if
                // the exchange stops reporting the ccy entirely.
                exchange_by_symbol.erase(st.symbol);
            }
        }
    }
    if (symbol_map.empty())
        return 0;  // nothing we care about on this exchange

    // Threshold: 1e4 in 1e8 scale = 0.0001 of a base unit (~$10 at
    // BTC prices, ~$0.40 at ETH prices). Smaller than the smallest
    // order_qty we place (0.0001 BTC); bigger than floating-point
    // rounding noise. Tune per-venue later if needed.
    constexpr int64_t kDivergenceThresholdE8 = 10000;  // 0.0001 base units

    const auto divergences = reconcile(positions_, exchange_by_symbol, exchange_id, symbol_map, kDivergenceThresholdE8);
    for (const auto& d : divergences) {
        bpt::common::log::warn(kLog(),
                               "RECONCILIATION DIVERGENCE instrument_id={} symbol='{}' "
                               "our_net_qty={:.8f} exchange_net_qty={:.8f} diff={:.8f}",
                               d.instrument_id,
                               d.exchange_symbol,
                               static_cast<double>(d.our_net_qty_e8) / 1e8,
                               static_cast<double>(d.exchange_net_qty_e8) / 1e8,
                               static_cast<double>(d.diff_e8) / 1e8);

        // Seed the tracker to the exchange view. The reconciler used
        // to only log, which silently left strategies quoting against
        // a stale inventory count across restarts — AS's inventory-
        // skew + max_inventory guards read a tracker saying "flat"
        // while the exchange had accumulated a real position from
        // prior sessions. See feedback_avoid_silent_divergence note
        // in project_prod_hardening_backlog.md.
        //
        // avg_entry_price is sourced from the same SBE row; for SPOT
        // symbols we derived qty from currency-balance delta rather
        // than a positions[] row, so there's no entry price to seed.
        // Pass 0.0 in that case — subsequent fills blend from 0-avg,
        // which produces wrong realized_pnl on any close of the
        // seeded portion but avoids fabricating an entry.
        double seed_avg_px = 0.0;
        if (const auto it = exchange_row_by_symbol.find(d.exchange_symbol); it != exchange_row_by_symbol.end()) {
            seed_avg_px = it->second.avg_entry_price;
        }
        positions_.seed(d.instrument_id, exchange_id, d.exchange_net_qty_e8, seed_avg_px);
        bpt::common::log::info(kLog(),
                               "reconciler: seeded position instrument_id={} symbol='{}' "
                               "to exchange view net_qty={:.8f} avg_price={:.4f}",
                               d.instrument_id,
                               d.exchange_symbol,
                               static_cast<double>(d.exchange_net_qty_e8) / 1e8,
                               seed_avg_px);
    }
    return divergences.size();
}

}  // namespace bpt::strategy::strategy
