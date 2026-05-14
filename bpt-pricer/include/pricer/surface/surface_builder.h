#pragma once

#include "pricer/pricing/forward_curve.h"
#include "pricer/surface/vol_surface_grid.h"

#include <messages/ExchangeId.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bpt::pricer::surface {

// Instrument metadata needed for IV computation.
struct OptionInstrument {
    uint64_t instrument_id;
    std::string underlying;
    std::string exchange;
    bpt::messages::ExchangeId::Value exchange_id;
    uint32_t expiry_date;  // YYYYMMDD
    double strike_price;
    bool is_call;
    std::string venue_symbol;  ///< e.g. "BTC-15MAY26-65000-C"; needed by MdSubscribeBatch
};

// Live market state for an option.
struct OptionMarketState {
    double bid_price{0.0};
    double ask_price{0.0};
    double mid_price{0.0};
    uint64_t last_update_ns{0};
};

// Builds IV surfaces from live market data and refdata.
// Groups option instruments by (exchange, underlying) and computes
// IV for each using Newton-Raphson on Black-Scholes.
class SurfaceBuilder {
public:
    SurfaceBuilder(double risk_free_rate, uint32_t newton_max_iter, double newton_tol);

    // Register an option instrument from refdata.
    void add_instrument(const OptionInstrument& inst);

    // Remove an instrument (e.g. on refdata delta REMOVE).
    void remove_instrument(uint64_t instrument_id);

    // Update BBO for an option instrument.
    void on_bbo(uint64_t instrument_id, double bid, double ask, uint64_t timestamp_ns);

    // Update the spot price for an underlying on an exchange (from spot BBO).
    void set_spot(const std::string& underlying, bpt::messages::ExchangeId::Value exchange_id, double spot_price);

    // Set a futures-implied forward price for an expiry.
    void set_forward(const std::string& underlying,
                     bpt::messages::ExchangeId::Value exchange_id,
                     uint32_t expiry_date,
                     double forward_price);

    // Build vol surfaces for all (exchange, underlying) pairs.
    // current_date is YYYYMMDD for T calculation.
    std::vector<VolSurfaceGrid> build(uint32_t current_date);

    // Number of registered option instruments.
    size_t instrument_count() const { return instruments_.size(); }

private:
    double risk_free_rate_;
    uint32_t newton_max_iter_;
    double newton_tol_;
    uint64_t next_seq_{1};

    // instrument_id → option metadata
    std::unordered_map<uint64_t, OptionInstrument> instruments_;

    // instrument_id → live market state
    std::unordered_map<uint64_t, OptionMarketState> market_;

    // Key: "exchange_id:underlying" → forward curve
    std::unordered_map<std::string, pricing::ForwardCurve> forward_curves_;

    static std::string curve_key(bpt::messages::ExchangeId::Value ex, const std::string& underlying);
};

}  // namespace bpt::pricer::surface
