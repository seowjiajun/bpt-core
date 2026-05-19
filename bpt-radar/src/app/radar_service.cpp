#include "radar/app/radar_service.h"

#include "radar/analysis/gex.h"
#include "radar/analysis/max_pain.h"
#include "radar/analysis/surface_analyzer.h"

#include <algorithm>
#include <bpt_common/logging.h>
#include <bpt_common/signal.h>
#include <bpt_common/util/tsc_clock.h>
#include <cmath>
#include <cstring>
#include <thread>

namespace bpt::radar {

RadarService::RadarService(config::Settings settings, messaging::RadarBus bus)
    : settings_(std::move(settings)),
      bus_(std::move(bus)) {
    // CRTP: templated subscribers dispatch directly into our on_* methods.
    bus_.surface_sub->set_handler(this);
    bus_.stats_sub->set_handler(this);
    bus_.funding_sub->set_handler(this);
    bus_.refdata_perp_sub->set_handler(this);
    bus_.trade_sub->set_handler(this);
    bus_.bbo_sub->set_handler(this);

    bpt::common::log::info(
        "[RadarService] ready — surface={} stats={} funding={} refdata={} md_data={} color={} publish={}ms",
        settings_.vol_surface.stream_id,
        settings_.instrument_stats.stream_id,
        settings_.funding_rate.stream_id,
        settings_.refdata_snapshot.stream_id,
        settings_.md_data.stream_id,
        settings_.market_color.stream_id,
        settings_.publish_interval_ms);
}

void RadarService::on_vol_surface(bpt::messages::VolSurface& surface) {
    // Pull the entire surface into our analysis-friendly POD vector. The SBE
    // iterator's state lives in the wrapped buffer — once we leave this
    // callback the next fragment can clobber it.
    SurfaceKey key{static_cast<uint8_t>(surface.exchangeId()), std::string(surface.getUnderlyingAsString())};

    std::vector<analysis::SurfacePoint> points;
    auto& sbe_points = surface.points();
    points.reserve(sbe_points.count());

    while (sbe_points.hasNext()) {
        sbe_points.next();
        analysis::SurfacePoint p;
        p.instrument_id = sbe_points.instrumentId();
        p.expiry_yyyymmdd = sbe_points.expiryDate();
        p.strike_price = sbe_points.strikePrice();
        p.option_side = static_cast<int>(sbe_points.optionSide());
        p.implied_vol = sbe_points.impliedVol();
        p.forward_price = sbe_points.forwardPrice();
        p.time_to_expiry_y = sbe_points.timeToExpiry();
        p.delta = sbe_points.delta();
        p.gamma = sbe_points.gamma();

        // Join OI if we've seen InstrumentStats for this instrument.
        auto it = oi_by_instrument_.find(p.instrument_id);
        if (it != oi_by_instrument_.end())
            p.open_interest = it->second;
        // else: leaves NaN default from SurfacePoint constructor.

        points.push_back(p);
    }

    surfaces_[key] = std::move(points);
}

void RadarService::on_instrument_stats(bpt::messages::InstrumentStats& stats) {
    const uint64_t id = stats.instrumentId();
    const double oi = stats.openInterest();
    if (std::isfinite(oi))
        oi_by_instrument_[id] = oi;

    // Capture mark + index for the perp-basis join in publish_for(). We don't
    // filter on instrument type here — keeping all rows is harmless and saves
    // a refdata round-trip on every InstrumentStats update.
    const double mark = stats.markPrice();
    const double index = stats.indexPrice();
    if (std::isfinite(mark) && std::isfinite(index) && index > 0.0) {
        auto& q = perp_quote_by_instrument_[id];
        q.mark = mark;
        q.index = index;
        q.last_seen_ns = bpt::common::util::TscClock::now_epoch_ns();
    }
}

void RadarService::on_funding(bpt::messages::FundingRate& fr) {
    FundingState s;
    // rateBps stores rate × 1e6 (per FundingRate SBE field convention).
    s.rate_8h = static_cast<double>(fr.rateBps()) / 1'000'000.0;
    s.next_funding_ts_ns = fr.nextFundingTs();
    funding_by_instrument_[fr.instrumentId()] = s;
}

void RadarService::on_refdata_perp(const messaging::api::RefdataPerpSubscriber::PerpInfo& p) {
    SurfaceKey key{p.exchange_id, p.underlying};
    perp_id_by_key_[key] = p.instrument_id;
    perp_key_by_id_[p.instrument_id] = key;
}

void RadarService::on_trade(bpt::messages::MdTrade& trade) {
    // Filter: keep only trades on perps we know about. Option/spot trades are
    // ignored at this stage (options-flow gets its own dedicated section
    // later).
    const uint64_t id = trade.instrumentId();
    auto kit = perp_key_by_id_.find(id);
    if (kit == perp_key_by_id_.end())
        return;

    const double price = trade.price();
    const double qty = trade.qty();
    if (!std::isfinite(price) || !std::isfinite(qty) || qty <= 0.0)
        return;

    TradeEntry e;
    e.ts_ns = bpt::common::util::TscClock::now_epoch_ns();
    e.buy = (trade.side() == bpt::messages::TradeSide::Value::BUY);
    e.notional = price * qty;

    auto& win = flow_window_by_key_[kit->second];
    win.push_back(e);

    // Prune anything older than the lookback. Cheap amortised — most calls
    // walk 0–1 entries.
    constexpr uint64_t kFlowWindowNs = 5ULL * 60ULL * 1'000'000'000ULL;  // 5 min
    while (!win.empty() && (e.ts_ns - win.front().ts_ns) > kFlowWindowNs)
        win.pop_front();
}

void RadarService::on_bbo(bpt::messages::MdMarketData& bbo) {
    // Same filter as trades — only perps we recognise. Mid is bid/ask midpoint;
    // skip crossed / one-sided books since they make realized vol blow up.
    const uint64_t id = bbo.instrumentId();
    auto kit = perp_key_by_id_.find(id);
    if (kit == perp_key_by_id_.end())
        return;

    const double bid = bbo.bidPrice();
    const double ask = bbo.askPrice();
    if (!std::isfinite(bid) || !std::isfinite(ask) || bid <= 0.0 || ask <= 0.0 || ask < bid)
        return;
    const double mid = 0.5 * (bid + ask);

    auto& win = mid_window_by_key_[kit->second];
    const uint64_t now_ns = bpt::common::util::TscClock::now_epoch_ns();

    // Throttle: store at most one sample per ~5s. BBO firehose runs many
    // updates per second on a live perp — keeping all of them would dominate
    // the RV calc with bid/ask bounce noise instead of price action.
    constexpr uint64_t kMinSampleSpacingNs = 5ULL * 1'000'000'000ULL;
    if (!win.empty() && (now_ns - win.back().ts_ns) < kMinSampleSpacingNs)
        return;

    win.push_back({now_ns, mid});

    // 1h rolling window — long enough to be statistically meaningful, short
    // enough to reflect the current regime.
    constexpr uint64_t kMidWindowNs = 3600ULL * 1'000'000'000ULL;
    while (!win.empty() && (now_ns - win.front().ts_ns) > kMidWindowNs)
        win.pop_front();
}

void RadarService::publish_all() {
    for (auto& [key, points] : surfaces_)
        publish_for(key, points);
}

void RadarService::publish_for(const SurfaceKey& key, std::vector<analysis::SurfacePoint>& cached) {
    if (cached.empty())
        return;

    // Refresh OI on each cached point — surface may have been written before
    // an OI update arrived for one of its strikes.
    for (auto& p : cached) {
        auto it = oi_by_instrument_.find(p.instrument_id);
        if (it != oi_by_instrument_.end())
            p.open_interest = it->second;
    }

    // Group strikes by expiry and find front/back.
    struct ExpiryBucket {
        uint32_t expiry_yyyymmdd;
        double time_to_expiry_y;
        std::vector<analysis::SurfacePoint> pts;
    };
    std::vector<ExpiryBucket> buckets;
    for (const auto& p : cached) {
        auto it = std::find_if(buckets.begin(), buckets.end(), [&](const ExpiryBucket& b) {
            return b.expiry_yyyymmdd == p.expiry_yyyymmdd;
        });
        if (it == buckets.end())
            buckets.push_back({p.expiry_yyyymmdd, p.time_to_expiry_y, {p}});
        else
            it->pts.push_back(p);
    }
    if (buckets.empty())
        return;
    std::sort(buckets.begin(), buckets.end(), [](const ExpiryBucket& a, const ExpiryBucket& b) {
        return a.expiry_yyyymmdd < b.expiry_yyyymmdd;
    });

    messaging::MarketColor color{};
    color.timestamp_ns = bpt::common::util::TscClock::now_epoch_ns();
    color.exchange_id = key.exchange_id;
    const std::size_t copy_len = std::min<std::size_t>(sizeof(color.underlying) - 1, key.underlying.size());
    std::memcpy(color.underlying, key.underlying.data(), copy_len);
    color.underlying[copy_len] = '\0';

    const auto& front = buckets.front();
    color.options_front_expiry_yyyymmdd = front.expiry_yyyymmdd;
    color.options_front_time_to_expiry_y = front.time_to_expiry_y;
    if (!front.pts.empty())
        color.options_front_forward_price = front.pts.front().forward_price;
    color.options_front_atm_iv = analysis::atm_iv(front.pts);
    color.options_front_rr_25d = analysis::risk_reversal_25d(front.pts);
    color.options_front_skew_slope = analysis::atm_skew_slope(front.pts);

    const auto& back = buckets.back();
    color.options_back_expiry_yyyymmdd = back.expiry_yyyymmdd;
    color.options_back_time_to_expiry_y = back.time_to_expiry_y;
    color.options_back_atm_iv = analysis::atm_iv(back.pts);

    if (std::isfinite(color.options_front_atm_iv) && std::isfinite(color.options_back_atm_iv))
        color.options_term_spread = color.options_back_atm_iv - color.options_front_atm_iv;

    // GEX is computed across the whole surface (all expiries, weighted equally).
    // Some shops weight by 1/T or 1/√T to reflect dealer-hedging focus on
    // near-dated gamma; we keep the raw sum for now and let consumers reweight.
    const auto gex_res = analysis::compute_gex(cached);
    color.options_gex = gex_res.gex;
    color.options_total_oi = gex_res.total_oi;
    color.options_strikes_with_oi = gex_res.strikes;

    // Max pain is per-expiry; the dominant signal is the front expiry's pin.
    color.options_max_pain_strike = analysis::max_pain_strike(front.pts);

    color.options_strike_count = static_cast<uint32_t>(cached.size());
    color.options_expiry_count = static_cast<uint32_t>(buckets.size());

    // Attach perp funding state if refdata gave us a perp_id for this
    // (exchange, underlying) AND md-gateway has pushed a FundingRate update
    // for that perp. Leaves NaN / 0 defaults otherwise — frontend renders "—".
    auto pid_it = perp_id_by_key_.find(key);
    if (pid_it != perp_id_by_key_.end()) {
        auto fr_it = funding_by_instrument_.find(pid_it->second);
        if (fr_it != funding_by_instrument_.end()) {
            color.perp_funding_rate_8h = fr_it->second.rate_8h;
            color.perp_next_funding_ts_ns = fr_it->second.next_funding_ts_ns;
        }

        // Basis join — mark and index from InstrumentStats. Drop to NaN when
        // the last quote is older than the staleness window so a frozen
        // md-gateway doesn't surface a phantom basis.
        constexpr uint64_t kQuoteStaleNs = 30'000'000'000ULL;  // 30s
        auto pq_it = perp_quote_by_instrument_.find(pid_it->second);
        if (pq_it != perp_quote_by_instrument_.end()) {
            const auto& q = pq_it->second;
            const uint64_t age_ns = color.timestamp_ns - q.last_seen_ns;
            if (q.last_seen_ns != 0 && age_ns < kQuoteStaleNs) {
                color.perp_mark_price = q.mark;
                color.perp_index_price = q.index;
                color.perp_basis_bps = (q.mark - q.index) / q.index * 1e4;
            }
        }
    }

    // Realized-vol window — walk mid samples, compute Σ r² over the actual
    // span of the deque, then annualise. Prune here in case no BBO has
    // arrived since the last publish.
    auto mid_it = mid_window_by_key_.find(key);
    if (mid_it != mid_window_by_key_.end()) {
        auto& mids = mid_it->second;
        constexpr uint64_t kMidWindowNs = 3600ULL * 1'000'000'000ULL;
        while (!mids.empty() && (color.timestamp_ns - mids.front().ts_ns) > kMidWindowNs)
            mids.pop_front();

        if (mids.size() >= 2) {
            double sum_sq = 0.0;
            for (size_t i = 1; i < mids.size(); ++i) {
                const double r = std::log(mids[i].mid / mids[i - 1].mid);
                sum_sq += r * r;
            }
            const double span_ns = static_cast<double>(mids.back().ts_ns - mids.front().ts_ns);
            if (span_ns > 0.0) {
                // Annualise: realized vol = sqrt(Σ r² × (year_seconds / window_seconds))
                constexpr double kYearSeconds = 365.25 * 86400.0;
                const double window_s = span_ns / 1e9;
                color.regime_realized_vol_1h = std::sqrt(sum_sq * (kYearSeconds / window_s));
            }
        }
        color.regime_sample_count = static_cast<uint32_t>(mids.size());
    }

    // Flow window — sum the buy/sell notional inside the 5min lookback. Prune
    // here too in case no MdTrade has arrived since the last publish.
    auto win_it = flow_window_by_key_.find(key);
    if (win_it != flow_window_by_key_.end()) {
        auto& win = win_it->second;
        constexpr uint64_t kFlowWindowNs = 5ULL * 60ULL * 1'000'000'000ULL;
        while (!win.empty() && (color.timestamp_ns - win.front().ts_ns) > kFlowWindowNs)
            win.pop_front();

        double buy_notional = 0.0;
        double sell_notional = 0.0;
        uint32_t count = 0;
        for (const auto& e : win) {
            if (e.buy)
                buy_notional += e.notional;
            else
                sell_notional += e.notional;
            ++count;
        }
        const double total = buy_notional + sell_notional;
        color.flow_buy_notional_5m = buy_notional;
        color.flow_sell_notional_5m = sell_notional;
        color.flow_imbalance_5m = total > 0.0 ? (buy_notional - sell_notional) / total : messaging::kNan;
        color.flow_trade_count_5m = count;
    }

    bus_.color_pub->publish(color);
}

void RadarService::run() {
    bpt::common::log::info("[RadarService] entering main loop");

    const auto interval = std::chrono::milliseconds(settings_.publish_interval_ms);
    auto last_publish = std::chrono::steady_clock::now() - interval;

    while (bpt::common::signal::is_running()) {
        int frags = 0;
        frags += bus_.surface_sub->poll();
        frags += bus_.stats_sub->poll();
        frags += bus_.funding_sub->poll();
        frags += bus_.refdata_perp_sub->poll();
        frags += bus_.trade_sub->poll();
        frags += bus_.bbo_sub->poll();

        auto now = std::chrono::steady_clock::now();
        if (now - last_publish >= interval) {
            publish_all();
            last_publish = now;
        }

        if (frags == 0)
            std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

}  // namespace bpt::radar
