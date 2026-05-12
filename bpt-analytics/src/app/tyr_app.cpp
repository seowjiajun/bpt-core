#include "analytics/app/tyr_app.h"

#include "analytics/messaging/toxicity_update.h"

#include <messages/ExecStatus.h>
#include <messages/ExecutionReport.h>
#include <messages/OrderSide.h>

#include <bpt_common/logging.h>
#include <bpt_common/signal.h>
#include <chrono>

namespace bpt::analytics {

using namespace bpt::messages;

AnalyticsApp::AnalyticsApp(config::Settings settings, messaging::AnalyticsBus bus)
    : settings_(std::move(settings)),
      bus_(std::move(bus)) {
    // Configure analysis components
    mt_cfg_ = {
        .horizon_1_ns = 1'000'000'000ULL,
        .horizon_2_ns = 5'000'000'000ULL,
        .horizon_3_ns = 30'000'000'000ULL,
        .max_pending = settings_.markout_max_pending,
    };
    ts_cfg_ = {
        .window_size = settings_.scorer_window_size,
        .window_duration_ns = settings_.scorer_window_duration_ns,
        .min_samples = settings_.scorer_min_samples,
    };
    fr_cfg_ = {
        .window_size = settings_.scorer_window_size,  // same window as scorer
    };

    bus_.exec_sub->on_report = [this](const ExecutionReport& rpt) {
        on_exec_report(rpt);
    };
    bus_.md_sub->on_bbo = [this](uint64_t instrument_id, double bid, double ask, uint64_t ts_ns) {
        on_bbo(instrument_id, bid, ask, ts_ns);
    };

    bpt::common::log::info("Exec report subscription ready: {} stream {}",
                           settings_.exec_report.channel,
                           settings_.exec_report.stream_id);
    bpt::common::log::info("MD subscription ready: {} stream {}",
                           settings_.md_data.channel,
                           settings_.md_data.stream_id);
    bpt::common::log::info("Toxicity publication ready: {} stream {}",
                           settings_.toxicity.channel,
                           settings_.toxicity.stream_id);
}

AnalyticsApp::InstrumentState& AnalyticsApp::get_or_create(uint64_t instrument_id) {
    auto it = state_.find(instrument_id);
    if (it != state_.end())
        return it->second;
    auto [inserted, ok] = state_.emplace(instrument_id, InstrumentState{mt_cfg_, ts_cfg_, fr_cfg_});
    return inserted->second;
}

void AnalyticsApp::on_bbo(uint64_t instrument_id, double bid, double ask, uint64_t timestamp_ns) {
    if (bid <= 0.0 || ask <= 0.0 || ask <= bid)
        return;

    const double mid = (bid + ask) * 0.5;
    auto& st = get_or_create(instrument_id);
    st.last_mid = mid;

    // Check markout horizons on every tick
    int completed = st.tracker.on_tick(mid, timestamp_ns);
    if (completed > 0) {
        auto observations = st.tracker.consume();
        for (const auto& obs : observations) {
            st.scorer.add(obs);
            bpt::common::log::info("Markout inst={} side={} fill_px={:.2f} 1s={:.1f}bps 5s={:.1f}bps 30s={:.1f}bps",
                                   obs.instrument_id,
                                   obs.side_sign > 0 ? "BUY" : "SELL",
                                   obs.fill_price,
                                   obs.markout_1s_bps,
                                   obs.markout_5s_bps,
                                   obs.markout_30s_bps);
        }
    }
}

void AnalyticsApp::on_exec_fill(uint64_t instrument_id, int side_sign, double fill_price, uint64_t timestamp_ns) {
    // Some exchanges (HL, OKX) send instrumentId=0 in exec reports.
    // Fall back to the last-seen mid from any instrument we're tracking.
    double mid = 0.0;
    if (auto it = state_.find(instrument_id); it != state_.end()) {
        mid = it->second.last_mid;
    } else if (instrument_id == 0 && !state_.empty()) {
        // Use the first (and typically only) instrument's mid
        mid = state_.begin()->second.last_mid;
    }

    auto& st = get_or_create(instrument_id == 0 && !state_.empty() ? state_.begin()->first : instrument_id);
    st.tracker.on_fill(st.last_mid > 0 ? instrument_id : 0, side_sign, fill_price, st.last_mid, timestamp_ns);
    bpt::common::log::info("Fill recorded: inst={} side={} px={:.2f} mid={:.2f}",
                           instrument_id,
                           side_sign > 0 ? "BUY" : "SELL",
                           fill_price,
                           st.last_mid);
}

void AnalyticsApp::on_exec_report(const ExecutionReport& rpt) {
    const auto status = rpt.status();
    const int side_sign = (rpt.side() == OrderSide::BUY) ? +1 : -1;
    const uint64_t now_ns = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    const uint64_t order_id = rpt.orderId();

    // Route instrument_id=0 to the first tracked instrument
    uint64_t inst_id = rpt.instrumentId();
    if (inst_id == 0 && !state_.empty())
        inst_id = state_.begin()->first;
    auto& st = get_or_create(inst_id);

    if (status == ExecStatus::ACKED) {
        st.fill_rate.on_acked(order_id, side_sign, now_ns);
    } else if (status == ExecStatus::FILLED || status == ExecStatus::PARTIAL) {
        const double price = static_cast<double>(rpt.price()) / 1e8;
        on_exec_fill(inst_id, side_sign, price, now_ns);
        st.fill_rate.on_filled(order_id, now_ns);
    } else if (status == ExecStatus::CANCELLED) {
        st.fill_rate.on_cancelled(order_id, now_ns);
    }
}

void AnalyticsApp::maybe_publish(uint64_t now_ns) {
    const uint64_t interval_ns = static_cast<uint64_t>(settings_.publish_interval_ms) * 1'000'000ULL;
    if (now_ns - last_publish_ns_ < interval_ns)
        return;
    last_publish_ns_ = now_ns;

    if (!bus_.tox_pub)
        return;

    for (const auto& [instrument_id, st] : state_) {
        // Pass 0 as instrument filter so observations with instrument_id=0
        // (from exchanges that don't carry canonical IDs in exec reports)
        // are included in the computation.
        auto update = st.scorer.compute(0, now_ns);
        update.instrument_id = instrument_id;

        // Add fill rate stats
        auto bid_fr = st.fill_rate.stats(+1);
        auto ask_fr = st.fill_rate.stats(-1);
        update.bid_fill_rate = bid_fr.fill_rate;
        update.ask_fill_rate = ask_fr.fill_rate;
        update.bid_ttf_ms = bid_fr.mean_ttf_ms;
        update.ask_ttf_ms = ask_fr.mean_ttf_ms;

        // Only publish if we have data on at least one side
        if (update.bid_sample_count == 0 && update.ask_sample_count == 0 && bid_fr.total == 0 && ask_fr.total == 0)
            continue;

        const bool sent = bus_.tox_pub->publish(update);

        if (sent) {
            // Copy packed fields to locals for fmt (can't bind references to packed members)
            double bid_m = update.bid_markout_5s_bps;
            uint32_t bid_n = update.bid_sample_count;
            double ask_m = update.ask_markout_5s_bps;
            uint32_t ask_n = update.ask_sample_count;
            double bid_t = update.bid_toxicity_score;
            double ask_t = update.ask_toxicity_score;
            double b_fr = update.bid_fill_rate;
            double a_fr = update.ask_fill_rate;
            double b_ttf = update.bid_ttf_ms;
            double a_ttf = update.ask_ttf_ms;
            bpt::common::log::info(
                "Published inst={} bid={:.1f}bps(n={}) ask={:.1f}bps(n={}) "
                "tox={:.2f}/{:.2f} fill_rate={:.0f}%/{:.0f}% ttf={:.0f}/{:.0f}ms",
                instrument_id,
                bid_m,
                bid_n,
                ask_m,
                ask_n,
                bid_t,
                ask_t,
                b_fr * 100,
                a_fr * 100,
                b_ttf,
                a_ttf);
        }
    }
}

void AnalyticsApp::run() {
    bpt::common::log::info("Starting main loop (publish every {}ms, window={} fills, min_samples={})",
                           settings_.publish_interval_ms,
                           settings_.scorer_window_size,
                           settings_.scorer_min_samples);

    while (bpt::common::signal::is_running()) {
        int frags = 0;
        if (bus_.exec_sub)
            frags += bus_.exec_sub->poll(10);
        if (bus_.md_sub)
            frags += bus_.md_sub->poll(10);

        const uint64_t now_ns = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        maybe_publish(now_ns);

        // Pause hint when idle — relinquishes pipeline resources to the SMT
        // sibling and avoids a hot busy-spin. Matches strategy_app.cpp's
        // pattern; analytics is downstream so a tighter spin here brings no
        // latency benefit.
        if (frags == 0)
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#elif defined(__aarch64__)
            asm volatile("yield" ::: "memory");
#endif
    }

    bpt::common::log::info("Shutting down");
}

}  // namespace bpt::analytics
