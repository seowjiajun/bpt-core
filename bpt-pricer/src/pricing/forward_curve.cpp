#include "pricer/pricing/forward_curve.h"

#include <cmath>
#include <ctime>

namespace bpt::pricer::pricing {

void ForwardCurve::set_spot(double spot) {
    spot_ = spot;
}

void ForwardCurve::set_risk_free_rate(double r) {
    r_ = r;
}

void ForwardCurve::set_forward(uint32_t expiry_date, double forward_price) {
    forwards_[expiry_date] = forward_price;
}

double ForwardCurve::get_forward(uint32_t expiry_date, uint32_t current_date) const {
    auto it = forwards_.find(expiry_date);
    if (it != forwards_.end()) {
        return it->second;
    }

    // Synthetic forward from spot
    const double T = time_to_expiry(expiry_date, current_date);
    if (T <= 0.0 || spot_ <= 0.0) {
        return spot_;
    }
    return spot_ * std::exp(r_ * T);
}

double ForwardCurve::time_to_expiry(uint32_t expiry_date, uint32_t current_date) {
    // Parse YYYYMMDD → days
    auto to_days = [](uint32_t d) -> int {
        const int y = static_cast<int>(d / 10000);
        const int m = static_cast<int>((d % 10000) / 100);
        const int day = static_cast<int>(d % 100);

        // Simplified Julian day number calculation
        const int a = (14 - m) / 12;
        const int y2 = y + 4800 - a;
        const int m2 = m + 12 * a - 3;
        return day + (153 * m2 + 2) / 5 + 365 * y2 + y2 / 4 - y2 / 100 + y2 / 400 - 32045;
    };

    const int days_diff = to_days(expiry_date) - to_days(current_date);
    return static_cast<double>(days_diff) / 365.0;
}

}  // namespace bpt::pricer::pricing
