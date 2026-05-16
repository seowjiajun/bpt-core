#include "pricer/surface/surface_builder.h"

#include "pricer/pricing/black_scholes.h"
#include "pricer/pricing/forward_curve.h"
#include "pricer/pricing/iv_solver.h"
#include "pricer/pricing/svi.h"

#include <cmath>
#include <set>
#include <unordered_set>

namespace bpt::pricer::surface {

SurfaceBuilder::SurfaceBuilder(double risk_free_rate, uint32_t newton_max_iter, double newton_tol)
    : risk_free_rate_(risk_free_rate),
      newton_max_iter_(newton_max_iter),
      newton_tol_(newton_tol) {}

void SurfaceBuilder::add_instrument(const OptionInstrument& inst) {
    instruments_[inst.instrument_id] = inst;
    // Ensure forward curve exists
    auto key = curve_key(inst.exchange_id, inst.underlying);
    if (forward_curves_.find(key) == forward_curves_.end()) {
        pricing::ForwardCurve fc;
        fc.set_risk_free_rate(risk_free_rate_);
        forward_curves_[key] = std::move(fc);
    }
}

void SurfaceBuilder::remove_instrument(uint64_t instrument_id) {
    instruments_.erase(instrument_id);
    market_.erase(instrument_id);
}

void SurfaceBuilder::on_bbo(uint64_t instrument_id, double bid, double ask, uint64_t timestamp_ns) {
    auto it = instruments_.find(instrument_id);
    if (it == instruments_.end())
        return;

    auto& ms = market_[instrument_id];
    ms.bid_price = bid;
    ms.ask_price = ask;
    ms.mid_price = (bid + ask) * 0.5;
    ms.last_update_ns = timestamp_ns;
}

void SurfaceBuilder::set_spot(const std::string& underlying,
                              bpt::messages::ExchangeId::Value exchange_id,
                              double spot_price) {
    auto key = curve_key(exchange_id, underlying);
    auto& fc = forward_curves_[key];
    fc.set_risk_free_rate(risk_free_rate_);
    fc.set_spot(spot_price);
}

void SurfaceBuilder::set_forward(const std::string& underlying,
                                 bpt::messages::ExchangeId::Value exchange_id,
                                 uint32_t expiry_date,
                                 double forward_price) {
    auto key = curve_key(exchange_id, underlying);
    auto& fc = forward_curves_[key];
    fc.set_risk_free_rate(risk_free_rate_);
    fc.set_forward(expiry_date, forward_price);
}

std::vector<VolSurfaceGrid> SurfaceBuilder::build(uint32_t current_date) {
    // Group instruments by (exchange_id, underlying)
    struct SurfaceKey {
        bpt::messages::ExchangeId::Value exchange_id;
        std::string underlying;
    };

    std::unordered_map<std::string, VolSurfaceGrid> grids;

    for (const auto& [id, inst] : instruments_) {
        auto mk = market_.find(id);
        if (mk == market_.end() || mk->second.mid_price <= 0.0) {
            continue;  // No market data yet
        }

        const auto& ms = mk->second;
        const auto key = curve_key(inst.exchange_id, inst.underlying);

        auto fc_it = forward_curves_.find(key);
        if (fc_it == forward_curves_.end())
            continue;

        const auto& fc = fc_it->second;
        const double fwd = fc.get_forward(inst.expiry_date, current_date);
        const double T = pricing::ForwardCurve::time_to_expiry(inst.expiry_date, current_date);

        if (T <= 0.0 || fwd <= 0.0)
            continue;

        // Deribit (and other inverse-quoted exchanges) quote option prices in the
        // underlying (e.g. BTC). Convert to USD for the IV solver by multiplying
        // by the forward price.  For USD-quoted exchanges this flag should be false.
        const bool inverse_quoted = (inst.exchange_id == bpt::messages::ExchangeId::DERIBIT);

        const double mid_px = inverse_quoted ? ms.mid_price * fwd : ms.mid_price;
        const double bid_px = inverse_quoted ? ms.bid_price * fwd : ms.bid_price;
        const double ask_px = inverse_quoted ? ms.ask_price * fwd : ms.ask_price;

        // Solve mid IV
        auto mid_iv = pricing::solve_iv(inst.is_call,
                                        mid_px,
                                        fwd,
                                        inst.strike_price,
                                        T,
                                        risk_free_rate_,
                                        newton_max_iter_,
                                        newton_tol_);

        if (!mid_iv)
            continue;

        // Optionally solve bid/ask IV
        double bid_iv = 0.0;
        double ask_iv = 0.0;
        if (bid_px > 0.0) {
            auto result = pricing::solve_iv(inst.is_call,
                                            bid_px,
                                            fwd,
                                            inst.strike_price,
                                            T,
                                            risk_free_rate_,
                                            newton_max_iter_,
                                            newton_tol_);
            if (result)
                bid_iv = result->iv;
        }
        if (ask_px > 0.0) {
            auto result = pricing::solve_iv(inst.is_call,
                                            ask_px,
                                            fwd,
                                            inst.strike_price,
                                            T,
                                            risk_free_rate_,
                                            newton_max_iter_,
                                            newton_tol_);
            if (result)
                ask_iv = result->iv;
        }

        // Compute Greeks from mid IV
        const auto greeks = inst.is_call ? pricing::bs_call(fwd, inst.strike_price, T, risk_free_rate_, mid_iv->iv)
                                         : pricing::bs_put(fwd, inst.strike_price, T, risk_free_rate_, mid_iv->iv);

        auto& grid = grids[key];
        if (grid.points.empty()) {
            grid.exchange_id = inst.exchange_id;
            grid.underlying = inst.underlying;
            grid.seq_num = next_seq_++;
        }

        grid.points.push_back(IvPoint{
            .instrument_id = inst.instrument_id,
            .expiry_date = inst.expiry_date,
            .strike_price = inst.strike_price,
            .option_side = inst.is_call ? bpt::messages::OptionSide::CALL : bpt::messages::OptionSide::PUT,
            .implied_vol = mid_iv->iv,
            .forward_price = fwd,
            .time_to_expiry = T,
            .bid_iv = bid_iv,
            .ask_iv = ask_iv,
            .bid_price = ms.bid_price,
            .ask_price = ms.ask_price,
            .delta = greeks.delta,
            .gamma = greeks.gamma,
            .vega = greeks.vega,
            .theta = greeks.theta,
        });
    }

    // ── SVI smile-fitting pass ────────────────────────────────────────────
    //
    // The observed-IV loop above only emits a surface point for options with
    // a venue BBO. Strikes without a maker quote are silent — strategies and
    // dashboards see a hole exactly when they need it most (orphan positions,
    // illiquid wings).
    //
    // Per-expiry SVI fit on observed (k, total_var) data gives us a continuous
    // IV(k) callable. We evaluate it at every refdata strike for the underlying
    // and emit an "interpolated" surface point. Bid/ask IV stay 0 to signal
    // "no market data; from model".
    //
    // Slices with <3 observed points are skipped (under-determined fit).
    for (auto& [key, grid] : grids) {
        // Collect observed (k, total_var) per expiry inside this grid.
        std::unordered_map<uint32_t, std::vector<pricing::SviFitInput>> by_expiry;
        std::unordered_set<uint64_t> observed_ids;
        observed_ids.reserve(grid.points.size());
        for (const auto& pt : grid.points) {
            observed_ids.insert(pt.instrument_id);
            const double k = std::log(pt.strike_price / pt.forward_price);
            const double total_var = pt.implied_vol * pt.implied_vol * pt.time_to_expiry;
            by_expiry[pt.expiry_date].push_back({k, total_var});
        }

        // Fit SVI per expiry slice.
        std::unordered_map<uint32_t, pricing::SviParams> fitted;
        for (const auto& [expiry, points] : by_expiry) {
            auto res = pricing::svi_fit(points);
            if (!res)
                continue;
            // Reject obvious mis-fits — rms residual much larger than typical
            // IV (e.g. > 0.5 total variance means the smile is broken).
            if (res->rms_residual > 0.5)
                continue;
            fitted[expiry] = res->params;
        }

        // Re-project observed points onto the fitted smile. Pre-fit, each
        // observed point's `implied_vol` and Greeks come from the raw venue
        // mid via Newton-Raphson — a per-strike IV with no cross-strike
        // smoothness. Strategies that anchor theo on this IV inherit any
        // venue-mid noise. Re-projecting onto the SVI fit gives every
        // observed point a smile-consistent IV (and Greeks); the dislocation
        // signal is still recoverable downstream as `implied_vol -
        // 0.5*(bid_iv + ask_iv)`, since bid_iv/ask_iv remain raw venue-derived.
        for (auto& pt : grid.points) {
            auto fit_it = fitted.find(pt.expiry_date);
            if (fit_it == fitted.end())
                continue;
            const double k = std::log(pt.strike_price / pt.forward_price);
            const double iv_fit = pricing::svi_iv(k, pt.time_to_expiry, fit_it->second);
            if (!std::isfinite(iv_fit) || iv_fit <= 0.0)
                continue;
            pt.implied_vol = iv_fit;
            const bool is_call = (pt.option_side == bpt::messages::OptionSide::CALL);
            const auto greeks = is_call
                ? pricing::bs_call(pt.forward_price, pt.strike_price, pt.time_to_expiry, risk_free_rate_, iv_fit)
                : pricing::bs_put(pt.forward_price, pt.strike_price, pt.time_to_expiry, risk_free_rate_, iv_fit);
            pt.delta = greeks.delta;
            pt.gamma = greeks.gamma;
            pt.vega = greeks.vega;
            pt.theta = greeks.theta;
        }

        // Find every refdata option matching this (exchange, underlying) and
        // emit an interpolated point if we don't already have an observed
        // one for it AND its expiry has a fitted slice.
        for (const auto& [iid, inst] : instruments_) {
            if (observed_ids.count(iid))
                continue;
            if (inst.exchange_id != grid.exchange_id || inst.underlying != grid.underlying)
                continue;
            auto fit_it = fitted.find(inst.expiry_date);
            if (fit_it == fitted.end())
                continue;

            const std::string fc_key = curve_key(inst.exchange_id, inst.underlying);
            auto fc_it = forward_curves_.find(fc_key);
            if (fc_it == forward_curves_.end())
                continue;
            const double fwd = fc_it->second.get_forward(inst.expiry_date, current_date);
            const double T = pricing::ForwardCurve::time_to_expiry(inst.expiry_date, current_date);
            if (T <= 0.0 || fwd <= 0.0)
                continue;

            const double k = std::log(inst.strike_price / fwd);
            const double iv = pricing::svi_iv(k, T, fit_it->second);
            if (!std::isfinite(iv) || iv <= 0.0)
                continue;

            const auto greeks = inst.is_call ? pricing::bs_call(fwd, inst.strike_price, T, risk_free_rate_, iv)
                                             : pricing::bs_put(fwd, inst.strike_price, T, risk_free_rate_, iv);

            grid.points.push_back(IvPoint{
                .instrument_id = inst.instrument_id,
                .expiry_date = inst.expiry_date,
                .strike_price = inst.strike_price,
                .option_side = inst.is_call ? bpt::messages::OptionSide::CALL : bpt::messages::OptionSide::PUT,
                .implied_vol = iv,
                .forward_price = fwd,
                .time_to_expiry = T,
                // bid_iv / ask_iv / bid_price / ask_price stay zero — sentinel
                // that this point is model-derived, not observed.
                .bid_iv = 0.0,
                .ask_iv = 0.0,
                .bid_price = 0.0,
                .ask_price = 0.0,
                .delta = greeks.delta,
                .gamma = greeks.gamma,
                .vega = greeks.vega,
                .theta = greeks.theta,
            });
        }
    }

    std::vector<VolSurfaceGrid> result;
    result.reserve(grids.size());
    for (auto& [_, grid] : grids) {
        result.push_back(std::move(grid));
    }
    return result;
}

std::string SurfaceBuilder::curve_key(bpt::messages::ExchangeId::Value ex, const std::string& underlying) {
    return std::to_string(static_cast<int>(ex)) + ":" + underlying;
}

}  // namespace bpt::pricer::surface
