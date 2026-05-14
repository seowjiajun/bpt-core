#pragma once

#include <bpt_app/base_settings.h>
#include <bpt_common/aeron/stream_config.h>
#include <cstdint>
#include <string>
#include <vector>

namespace bpt::pricer::config {

struct Settings {
    // Shared lifecycle config (environment, media_driver_dir, logging,
    // metrics_port, calibrate_tsc). Populated by bpt::app::load_base_settings().
    bpt::app::BaseSettings base;

    // MD input (reads from MdGateway stream 2002 = md_data)
    bpt::common::config::StreamConfig md_data;

    // MD control (pricer → md-gateway: subscribe requests for the option
    // universe we want BBOs for). Reuses the same stream strategy uses;
    // md-gateway refcounts across consumers so pricer and strategy can
    // coexist.
    bpt::common::config::StreamConfig md_control;

    // Refdata input (reads from Sindri streams 1001-1003)
    bpt::common::config::StreamConfig refdata_snapshot;
    bpt::common::config::StreamConfig refdata_delta;
    bpt::common::config::StreamConfig refdata_control;  // Strategy → Sindri (we reuse for our subscription)

    // Vol surface output (Pricer → Strategy stream 4001)
    bpt::common::config::StreamConfig vol_surface;

    // Heartbeat + ready output (Pricer → Strategy stream 4002 = pricer_status)
    bpt::common::config::StreamConfig pricer_status;

    // Exchanges to track options on
    std::vector<std::string> exchanges;

    // Underlyings to compute surfaces for (e.g. "BTC", "ETH")
    std::vector<std::string> underlyings;

    // How often (ms) to recompute and publish the full surface
    uint32_t publish_interval_ms{2000};

    // Risk-free rate for Black-Scholes (annualised)
    double risk_free_rate{0.05};

    // IV solver settings
    uint32_t newton_max_iterations{100};
    double newton_tolerance{1e-8};

    /// \brief Universe filter — narrows the option set pricer subscribes to.
    ///
    /// Deribit lists 1000+ strikes per underlying across many expiries; the
    /// majority are deep OTM/ITM at far-dated expiries where mark prices are
    /// stale and IV computation has no edge. The filter restricts pricer's
    /// MdSubscribeBatch to options worth surfacing.
    ///
    /// front_n_expiries: take the N closest non-expired expiry dates per
    ///                   underlying. 0 disables (subscribe to everything,
    ///                   not recommended on Deribit).
    /// max_strikes_per_expiry: cap per-expiry strikes after the band filter
    ///                         applies. Belt-and-suspenders against an
    ///                         extreme tail of OTM strikes. 0 = unlimited.
    ///
    /// At init pricer doesn't yet know the forward, so the strike-band
    /// filter (within ±X% of forward) is best-effort: when forward becomes
    /// known via spot/perp BBO, pricer can re-subscribe with the narrower
    /// set. For now the expiry filter alone narrows ~2000 strikes to a few
    /// hundred — workable.
    struct UniverseFilter {
        uint32_t front_n_expiries{4};
        uint32_t max_strikes_per_expiry{0};  // 0 = unlimited
    } universe;
};

Settings load(const std::string& path);

}  // namespace bpt::pricer::config
