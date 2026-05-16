#include "pricer/pricing/svi.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using bpt::pricer::pricing::SviFitInput;
using bpt::pricer::pricing::SviParams;
using bpt::pricer::pricing::svi_fit;
using bpt::pricer::pricing::svi_iv;
using bpt::pricer::pricing::svi_total_variance;

namespace {

// Generate observed points from a known SVI slice + small noise. Tests that
// the fitter recovers the params within a tolerance.
std::vector<SviFitInput> synth_observations(const SviParams& true_params,
                                            const std::vector<double>& ks) {
    std::vector<SviFitInput> out;
    out.reserve(ks.size());
    for (double k : ks)
        out.push_back({k, svi_total_variance(k, true_params)});
    return out;
}

}  // namespace

TEST(SviEval, MonotonicWingsForFlatSmile) {
    // a=0.04, b=0.1, ρ=0, m=0, s=0.1 — symmetric, wings open upward.
    SviParams p{.a = 0.04, .b = 0.1, .rho = 0.0, .m = 0.0, .s = 0.1};
    const double w_atm = svi_total_variance(0.0, p);
    const double w_left = svi_total_variance(-0.5, p);
    const double w_right = svi_total_variance(0.5, p);
    EXPECT_LT(w_atm, w_left);
    EXPECT_LT(w_atm, w_right);
    EXPECT_NEAR(w_left, w_right, 1e-12);  // symmetric
}

TEST(SviEval, NegativeSkewLowerLeftWingHigher) {
    // Negative rho → left wing (low strikes / puts) higher than right.
    SviParams p{.a = 0.04, .b = 0.1, .rho = -0.5, .m = 0.0, .s = 0.1};
    const double w_left = svi_total_variance(-0.5, p);
    const double w_right = svi_total_variance(0.5, p);
    EXPECT_GT(w_left, w_right);
}

TEST(SviEval, IvFromTotalVariance) {
    SviParams p{.a = 0.04, .b = 0.1, .rho = 0.0, .m = 0.0, .s = 0.1};
    // At ATM with these params, w(0) = a + b·s = 0.05 → for T=1, σ = sqrt(0.05) ≈ 0.2236
    const double T = 1.0;
    const double iv = svi_iv(0.0, T, p);
    EXPECT_NEAR(iv, std::sqrt(0.05), 1e-9);
}

TEST(SviFit, RecoversKnownParamsFromCleanData) {
    SviParams truth{.a = 0.02, .b = 0.15, .rho = -0.4, .m = 0.05, .s = 0.12};
    const std::vector<double> ks = {-0.4, -0.2, -0.1, 0.0, 0.1, 0.2, 0.3, 0.4};
    auto pts = synth_observations(truth, ks);

    auto res = svi_fit(pts);
    ASSERT_TRUE(res.has_value());
    EXPECT_TRUE(res->converged) << "iterations=" << res->iterations
                                << " rms=" << res->rms_residual;

    // Recovered params should fit the data well — even if the params
    // themselves differ slightly (SVI has redundancy in some directions),
    // w(k) at the observation points should match closely.
    for (const auto& pt : pts) {
        const double w_fit = svi_total_variance(pt.k, res->params);
        EXPECT_NEAR(w_fit, pt.total_var, 1e-4)
            << "k=" << pt.k << " obs=" << pt.total_var << " fit=" << w_fit;
    }
    EXPECT_LT(res->rms_residual, 1e-4);
}

TEST(SviFit, RejectsUnderdeterminedInput) {
    // Two points → 5-param fit impossible.
    std::vector<SviFitInput> pts = {{-0.1, 0.05}, {0.1, 0.05}};
    auto res = svi_fit(pts);
    EXPECT_FALSE(res.has_value());
}

TEST(SviFit, ExtrapolatesPastObservedKs) {
    // Fit on a narrow window; verify w(k) outside the window stays positive
    // and increases (wings open upward). This is the load-bearing property
    // for filling sparse-book strikes.
    SviParams truth{.a = 0.02, .b = 0.15, .rho = -0.4, .m = 0.0, .s = 0.1};
    const std::vector<double> observed_ks = {-0.05, -0.02, 0.0, 0.02, 0.05};
    auto pts = synth_observations(truth, observed_ks);

    auto res = svi_fit(pts);
    ASSERT_TRUE(res.has_value());

    const double w_far_otm_call = svi_total_variance(0.5, res->params);
    const double w_far_otm_put = svi_total_variance(-0.5, res->params);
    const double w_atm = svi_total_variance(0.0, res->params);

    EXPECT_GT(w_far_otm_call, w_atm);
    EXPECT_GT(w_far_otm_put, w_atm);
    EXPECT_GT(w_far_otm_call, 0.0);
    EXPECT_GT(w_far_otm_put, 0.0);
}
