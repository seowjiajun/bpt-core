#pragma once

#include <cstdint>
#include <map>
#include <optional>

namespace bpt::pricer::pricing {

// Simple forward curve: maps expiry date (YYYYMMDD) to forward price.
// Built from futures prices when available; falls back to spot * exp(r*T).
class ForwardCurve {
public:
    // Set the spot price (used as fallback when no futures price is available).
    void set_spot(double spot);

    // Set the risk-free rate for synthetic forward calculation.
    void set_risk_free_rate(double r);

    // Add a futures-implied forward price for a given expiry.
    void set_forward(uint32_t expiry_date, double forward_price);

    // Get the forward price for a given expiry.
    // If no futures price is available, uses spot * exp(r * T).
    // current_date is YYYYMMDD, used to compute T.
    double get_forward(uint32_t expiry_date, uint32_t current_date) const;

    // Compute time-to-expiry in years (ACT/365) from YYYYMMDD dates.
    static double time_to_expiry(uint32_t expiry_date, uint32_t current_date);

private:
    double spot_{0.0};
    double r_{0.05};
    std::map<uint32_t, double> forwards_;  // expiry_date → forward price
};

}  // namespace bpt::pricer::pricing
