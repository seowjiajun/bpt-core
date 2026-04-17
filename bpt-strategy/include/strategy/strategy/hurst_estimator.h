#pragma once

// Rolling Hurst-exponent estimators used by regime-switching
// strategies. Pure, stateless functions — no I/O, no trading state,
// no config objects. The caller supplies the return series and the
// effective window cap.
//
// A Hurst value H classifies short-horizon behaviour of a price
// series:
//   H < 0.5  → mean-reverting (anti-persistent)
//   H ≈ 0.5  → random walk
//   H > 0.5  → trending (persistent)
//
// Both functions return 0.5 on insufficient data (< 20 samples) or
// degenerate inputs (zero variance, zero range), and clamp the
// output to [0.0, 1.0].

#include <cstddef>

namespace bpt::strategy::strategy {

// Single-window rescaled-range Hurst estimator over the first
// `count` entries of `returns`. The effective window is
// min(count, max_window). Pointer + length form rather than
// templated array so unit tests can feed ad-hoc sequences.
[[nodiscard]] double compute_hurst(const double* returns, std::size_t count, std::size_t max_window);

// Multi-window R/S with a linear regression of log(R/S) vs log(n)
// over sub-window sizes 8, 16, 32, ... up to n/2. The regression
// slope is the Hurst exponent — substantially more robust than the
// single-window estimator on short series. Falls back to
// compute_hurst() when fewer than 2 valid regression points exist.
[[nodiscard]] double compute_hurst_multi_window(const double* returns,
                                                  std::size_t count,
                                                  std::size_t max_window);

}  // namespace bpt::strategy::strategy
