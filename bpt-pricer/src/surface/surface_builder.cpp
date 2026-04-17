#include "pricer/surface/surface_builder.h"

#include "pricer/pricing/black_scholes.h"
#include "pricer/pricing/forward_curve.h"
#include "pricer/pricing/iv_solver.h"


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
