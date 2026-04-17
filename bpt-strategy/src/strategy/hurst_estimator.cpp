#include "strategy/strategy/hurst_estimator.h"

#include <algorithm>
#include <cmath>

namespace bpt::strategy::strategy {

double compute_hurst(const double* returns, std::size_t count, std::size_t max_window) {
    const std::size_t n = std::min(count, max_window);
    if (n < 20)
        return 0.5;

    // Mean.
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i)
        sum += returns[i];
    const double mean = sum / static_cast<double>(n);

    // Cumulative deviations + sum of squares.
    double cum = 0.0;
    double max_cum = -1e30;
    double min_cum = 1e30;
    double sq_sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double dev = returns[i] - mean;
        cum += dev;
        if (cum > max_cum)
            max_cum = cum;
        if (cum < min_cum)
            min_cum = cum;
        sq_sum += dev * dev;
    }

    const double R = max_cum - min_cum;
    const double S = std::sqrt(sq_sum / static_cast<double>(n));

    if (S < 1e-15 || R < 1e-15)
        return 0.5;

    const double H = std::log(R / S) / std::log(static_cast<double>(n));
    return std::clamp(H, 0.0, 1.0);
}

double compute_hurst_multi_window(const double* returns, std::size_t count, std::size_t max_window) {
    const std::size_t n = std::min(count, max_window);
    if (n < 20)
        return 0.5;

    double sum_x = 0.0, sum_y = 0.0, sum_xx = 0.0, sum_xy = 0.0;
    int num_points = 0;

    for (std::size_t win = 8; win <= n / 2; win *= 2) {
        const std::size_t num_chunks = n / win;
        if (num_chunks == 0)
            continue;

        double rs_sum = 0.0;
        int valid_chunks = 0;

        for (std::size_t c = 0; c < num_chunks; ++c) {
            const std::size_t start = c * win;

            double chunk_sum = 0.0;
            for (std::size_t i = 0; i < win; ++i)
                chunk_sum += returns[start + i];
            const double chunk_mean = chunk_sum / static_cast<double>(win);

            double cum = 0.0, max_cum = -1e30, min_cum = 1e30, sq = 0.0;
            for (std::size_t i = 0; i < win; ++i) {
                const double dev = returns[start + i] - chunk_mean;
                cum += dev;
                if (cum > max_cum)
                    max_cum = cum;
                if (cum < min_cum)
                    min_cum = cum;
                sq += dev * dev;
            }

            const double R = max_cum - min_cum;
            const double S = std::sqrt(sq / static_cast<double>(win));

            if (S > 1e-15 && R > 1e-15) {
                rs_sum += R / S;
                ++valid_chunks;
            }
        }

        if (valid_chunks == 0)
            continue;

        const double avg_rs = rs_sum / static_cast<double>(valid_chunks);
        const double log_n = std::log(static_cast<double>(win));
        const double log_rs = std::log(avg_rs);

        sum_x += log_n;
        sum_y += log_rs;
        sum_xx += log_n * log_n;
        sum_xy += log_n * log_rs;
        ++num_points;
    }

    if (num_points < 2) {
        // Fall back to single-window if we couldn't build a regression.
        return compute_hurst(returns, count, max_window);
    }

    // Linear regression: slope = (n*sum_xy - sum_x*sum_y) / (n*sum_xx - sum_x²)
    const double denom = num_points * sum_xx - sum_x * sum_x;
    if (std::abs(denom) < 1e-15)
        return 0.5;

    const double H = (num_points * sum_xy - sum_x * sum_y) / denom;
    return std::clamp(H, 0.0, 1.0);
}

}  // namespace bpt::strategy::strategy
