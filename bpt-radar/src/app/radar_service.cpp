#include "radar/app/radar_service.h"

#include "radar/analysis/gex.h"
#include "radar/analysis/max_pain.h"
#include "radar/analysis/surface_analyzer.h"

#include <bpt_common/logging.h>
#include <bpt_common/signal.h>
#include <bpt_common/util/tsc_clock.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace bpt::radar {

RadarService::RadarService(config::Settings settings, messaging::RadarBus bus)
    : settings_(std::move(settings)),
      bus_(std::move(bus)) {
    bus_.surface_sub->on_vol_surface = [this](bpt::messages::VolSurface& s) { on_vol_surface(s); };
    bus_.stats_sub->on_stats = [this](bpt::messages::InstrumentStats& s) { on_instrument_stats(s); };

    bpt::common::log::info("[RadarService] ready — surface stream={} stats stream={} color stream={} publish={}ms",
                           settings_.vol_surface.stream_id,
                           settings_.instrument_stats.stream_id,
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
    const double oi = stats.openInterest();
    if (std::isfinite(oi))
        oi_by_instrument_[stats.instrumentId()] = oi;
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
    color.front_expiry_yyyymmdd = front.expiry_yyyymmdd;
    color.front_time_to_expiry_y = front.time_to_expiry_y;
    if (!front.pts.empty())
        color.front_forward_price = front.pts.front().forward_price;
    color.front_atm_iv = analysis::atm_iv(front.pts);
    color.front_rr_25d = analysis::risk_reversal_25d(front.pts);
    color.front_skew_slope = analysis::atm_skew_slope(front.pts);

    const auto& back = buckets.back();
    color.back_expiry_yyyymmdd = back.expiry_yyyymmdd;
    color.back_time_to_expiry_y = back.time_to_expiry_y;
    color.back_atm_iv = analysis::atm_iv(back.pts);

    if (std::isfinite(color.front_atm_iv) && std::isfinite(color.back_atm_iv))
        color.term_spread = color.back_atm_iv - color.front_atm_iv;

    // GEX is computed across the whole surface (all expiries, weighted equally).
    // Some shops weight by 1/T or 1/√T to reflect dealer-hedging focus on
    // near-dated gamma; we keep the raw sum for now and let consumers reweight.
    const auto gex_res = analysis::compute_gex(cached);
    color.gex = gex_res.gex;
    color.total_oi = gex_res.total_oi;
    color.strikes_with_oi = gex_res.strikes;

    // Max pain is per-expiry; the dominant signal is the front expiry's pin.
    color.max_pain_strike = analysis::max_pain_strike(front.pts);

    color.strike_count = static_cast<uint32_t>(cached.size());
    color.expiry_count = static_cast<uint32_t>(buckets.size());

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
