#pragma once

#include <messages/ExchangeId.h>
#include <messages/OptionSide.h>

#include <cstdint>
#include <string>
#include <vector>

namespace bpt::pricer::surface {

// A single IV point on the surface.
struct IvPoint {
    uint64_t instrument_id;
    uint32_t expiry_date;  // YYYYMMDD
    double strike_price;
    bpt::messages::OptionSide::Value option_side;
    double implied_vol;     // Mid IV (annualised)
    double forward_price;   // Forward used for computation
    double time_to_expiry;  // Years (ACT/365)
    double bid_iv;          // 0.0 if unavailable
    double ask_iv;          // 0.0 if unavailable
    // Option BBO — same tick used to compute IV
    double bid_price;
    double ask_price;
    // Greeks (Black-Scholes, annualised)
    double delta;
    double gamma;
    double vega;
    double theta;
};

// Vol surface for a single underlying on a single exchange.
struct VolSurfaceGrid {
    bpt::messages::ExchangeId::Value exchange_id;
    std::string underlying;  // e.g. "BTC"
    uint64_t seq_num{0};
    std::vector<IvPoint> points;

    void clear() { points.clear(); }
};

}  // namespace bpt::pricer::surface
