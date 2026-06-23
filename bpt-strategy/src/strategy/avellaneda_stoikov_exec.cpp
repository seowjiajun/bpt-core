// AS execution-report handling: position/queue updates, post-fill markout
// arming, drawdown pause, shutdown-unwind retries, exchange-error backoff.

#include "strategy/clock/sim_clock.h"
#include "strategy/strategy/avellaneda_stoikov_strategy.h"
#include "strategy/unwind/graceful_unwinder.h"

#include <messages/ExecStatus.h>
#include <messages/RejectReason.h>
#include <messages/RejectSource.h>
#include <messages/TradeSide.h>

#include <bpt_common/logging.h>
#include <bpt_common/util/tsc_clock.h>
#include <cmath>

using bpt::messages::ExecStatus;
using bpt::messages::OrderSide;
using bpt::messages::RejectSource;

namespace bpt::strategy::strategy {

namespace {
quill::Logger* kLog() {
    static quill::Logger* l = bpt::common::logging::get_logger("AS");
    return l;
}
}  // namespace

void AvellanedaStoikovStrategy::on_exec_report(const bpt::messages::ExecutionReport& rpt) {
    // OM owns lifecycle — let it update OrderState before we dispatch.
    order_mgr_->on_exec_report(rpt);

    const uint64_t order_id = rpt.orderId();
    const auto status = rpt.status();

    auto handle = order_mgr_->find_by_id(order_id);
    if (!handle.valid())
        return;
    order::OrderState* const os = handle.state;
    const uint64_t canonical_id = os->instrument_id;

    auto* st_ptr = find_state(canonical_id);
    if (!st_ptr) {
        bpt::common::log::warn(kLog(), "ExecReport order_id={} instrument_id={} not in state — dropping",
                               order_id, canonical_id);
        return;
    }

    InstrumentState& st = *st_ptr;

    // Shutdown-drain orders: update position then hand off to the unwinder.
    if (os->tag == unwind::GracefulUnwinder::kTag) {
        if (status == ExecStatus::FILLED || status == ExecStatus::PARTIAL)
            positions_.on_fill(canonical_id, st.exchange_id, rpt.side(), rpt.filledQty(), rpt.price());
        unwinder_.on_exec_report(rpt);
        return;
    }

    if (status == ExecStatus::ACKED) {
        bpt::common::log::debug(kLog(), "ExecReport order_id={} {} {} ACKED", order_id, st.symbol, st.exchange);
    } else if (status == ExecStatus::REJECTED) {
        const auto src = rpt.rejectSource();
        const bool gateway_reject = (src == RejectSource::GATEWAY || src == RejectSource::RISK);
        if (gateway_reject)
            bpt::common::log::error(kLog(),
                                    "ExecReport order_id={} {} {} REJECTED reason={} source={}",
                                    order_id,
                                    st.symbol,
                                    st.exchange,
                                    bpt::messages::RejectReason::c_str(rpt.rejectReason()),
                                    bpt::messages::RejectSource::c_str(src));
        else
            bpt::common::log::warn(kLog(),
                                   "ExecReport order_id={} {} {} REJECTED reason={} source={}",
                                   order_id,
                                   st.symbol,
                                   st.exchange,
                                   bpt::messages::RejectReason::c_str(rpt.rejectReason()),
                                   bpt::messages::RejectSource::c_str(src));
    } else {
        bpt::common::log::info(kLog(),
                               "ExecReport order_id={} {} {} status={} filled={:.6f} price={:.2f}",
                               order_id,
                               st.symbol,
                               st.exchange,
                               bpt::messages::ExecStatus::c_str(status),
                               static_cast<double>(rpt.filledQty()) / 1e8,
                               static_cast<double>(rpt.price()) / 1e8);
    }

    if (status == ExecStatus::FILLED || status == ExecStatus::PARTIAL) {
        // Capture rpnl baseline before the fill so we can derive this
        // fill's CONTRIBUTION to realized PnL (PositionTracker holds
        // session-cumulative; γ-feedback wants per-fill delta). When
        // there's no prior position, baseline = 0.
        const auto before = positions_.get(canonical_id, st.exchange_id);
        const double prior_rpnl = before ? before->realized_pnl : 0.0;

        positions_.on_fill(canonical_id, st.exchange_id, rpt.side(), rpt.filledQty(), rpt.price());
        st.queue.on_fill(order_id, static_cast<double>(rpt.filledQty()) / 1e8);

        // Record fill price for markout evaluation on next BBO tick (passive fills only;
        // unwind orders are intentionally aggressive — exclude so they don't trip the cooldown).
        if (post_fill_markout_threshold_bps_ < 0.0 && os->tag == kTagQuote) {
            const double fill_px = static_cast<double>(rpt.price()) / 1e8;
            if (rpt.side() == bpt::messages::TradeSide::BUY) {
                st.pending_buy_fill_price = fill_px;
                st.pending_buy_fill_ts = st.last_tick_ns;
            } else {
                st.pending_sell_fill_price = fill_px;
                st.pending_sell_fill_ts = st.last_tick_ns;
            }
        }

        if (const auto pos = positions_.get(canonical_id, st.exchange_id)) {
            bpt::common::log::info(kLog(),
                                   "Position {} @ {}  net_qty={:.6f}  avg_price={:.2f}  rpnl={:.4f}",
                                   st.symbol,
                                   st.exchange,
                                   static_cast<double>(pos->net_qty) / 1e8,
                                   pos->avg_price,
                                   pos->realized_pnl);

            // γ-feedback rolling window — only push when feature enabled
            // and the fill actually realized PnL (opening fills don't
            // realize anything; deltas of 0 would dilute the window).
            if (pricer_.config().gamma_pnl_window_n > 0) {
                const double delta = pos->realized_pnl - prior_rpnl;
                if (delta != 0.0) {
                    st.recent_rpnl.push_back(delta);
                    while (st.recent_rpnl.size() > pricer_.config().gamma_pnl_window_n)
                        st.recent_rpnl.pop_front();
                }
            }

            // Drawdown circuit-breaker. Trigger when realized PnL crosses
            // the configured loss threshold AND we're not already in a
            // pause window. The pause_active flag in SuppressionState
            // prevents NEW quotes; we ALSO have to actively cancel any
            // resting bid/ask here — otherwise pre-existing live orders
            // sit in the book and keep filling during the pause window.
            // (Same pattern as the vol_halted cancel block in on_bbo:
            // suppression alone doesn't pull live orders, only stops
            // requotes; explicit cancel is required.)
            if (pause_below_rpnl_usd_ < 0.0 && pos->realized_pnl < pause_below_rpnl_usd_ &&
                prior_rpnl >= pause_below_rpnl_usd_) {
                const uint64_t now_ns = bpt::common::util::TscClock::now_epoch_ns();
                st.pause_until_ns = now_ns + static_cast<uint64_t>(pause_cooldown_s_ * 1e9);
                bpt::common::log::warn(kLog(),
                                       "{} PAUSE TRIGGERED rpnl={:.4f} crossed below threshold={:.4f} — "
                                       "halting both sides for {:.0f}s",
                                       st.symbol,
                                       pos->realized_pnl,
                                       pause_below_rpnl_usd_,
                                       pause_cooldown_s_);
                if (order_mgr_) {
                    if (st.h_bid.live())
                        order_mgr_->send_cancel(st.h_bid);
                    if (st.h_ask.live())
                        order_mgr_->send_cancel(st.h_ask);
                }
            }
        }
    }

    // Terminal statuses: drop our handle so the next requote can place a fresh order.
    // The OrderState itself stays in OM's store_ for the process lifetime.
    if (status == ExecStatus::FILLED || status == ExecStatus::CANCELLED || status == ExecStatus::REJECTED) {
        st.queue.on_cancel(order_id);
        if (st.h_bid.state == os)
            st.h_bid.reset();
        else if (st.h_ask.state == os)
            st.h_ask.reset();
        else if (st.h_unwind.state == os)
            st.h_unwind.reset();
    }
    // PARTIAL: order still live — handles untouched.
    // ACKED:   acknowledged but not yet filled — handles untouched.

    // Exchange-error backoff: consecutive EXCHANGE-sourced rejections trigger
    // increasing cooldowns so we don't flood a broken/unfunded account.
    if (status == ExecStatus::REJECTED && rpt.rejectSource() == RejectSource::EXCHANGE) {
        ++st.consecutive_exchange_errors;
        const uint64_t backoff_s = (st.consecutive_exchange_errors == 1)   ? 5
                                   : (st.consecutive_exchange_errors == 2) ? 15
                                                                           : 30;
        const uint64_t now_ns = bpt::strategy::clock::SimClock::now_ns();
        st.reject_backoff_until_ns = now_ns + backoff_s * 1'000'000'000ULL;
        bpt::common::log::warn(kLog(),
                               "Exchange rejection backoff {} @ {}: {}s (consecutive={})",
                               st.symbol,
                               st.exchange,
                               backoff_s,
                               st.consecutive_exchange_errors);
    } else if (status == ExecStatus::ACKED) {
        st.consecutive_exchange_errors = 0;
        st.reject_backoff_until_ns = 0;
    }
}

}  // namespace bpt::strategy::strategy
