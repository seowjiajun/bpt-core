#pragma once

// BaseSettings — the minimal lifecycle configuration every bpt-core service
// shares. Each service's Settings struct embeds one of these as a member
// named `base`, populates it via load_base_settings() in the service's
// config loader, and reads its own domain-specific fields alongside.
//
// Keeping this struct lean is deliberate: lifecycle concerns (signal, TSC,
// logging, Aeron, metrics port) are universal; everything else (Aeron stream
// IDs, adapter lists, risk thresholds, strategy params) stays service-local.

#include <bpt_common/env.h>
#include <bpt_common/logging.h>
#include <cstdint>
#include <string>
#include <toml++/toml.hpp>

namespace bpt::app {

// Env lives in bpt-common so primitives (secrets_client, future env-aware
// helpers) can consume it without a circular dep on bpt-app. Aliased
// into bpt::app:: so existing call sites like `bpt::app::Env::PROD`
// keep compiling.
using Env = bpt::common::Env;
using bpt::common::env_from_string;
using bpt::common::to_string;

struct BaseSettings {
    // Deployment environment. Default DEV is the safest fallback (no
    // prod-specific behaviour triggers). Loader will override from TOML
    // and throw if the TOML value isn't a recognised name.
    Env environment{Env::DEV};

    [[nodiscard]] bool is_prod() const noexcept { return environment == Env::PROD; }
    [[nodiscard]] bool is_qa() const noexcept { return environment == Env::QA; }
    [[nodiscard]] bool is_dev() const noexcept { return environment == Env::DEV; }

    // Aeron MediaDriver IPC directory. Empty = use Aeron's default.
    std::string media_driver_dir;

    // Quill logging config — level, dir, rotation, flush behaviour.
    bpt::common::logging::LogConfig logging;

    // Prometheus metrics HTTP port. 0 = exposer disabled. Each service still
    // constructs its own typed metrics struct from this port; bpt-app just
    // reads the shared TOML key.
    uint16_t metrics_port{0};

    // TSC calibration is a one-time cost (~50ms) to map the invariant-TSC
    // clock to wall-clock epoch ns. Required for any service that uses
    // bpt::common::util::TscClock::now_epoch_ns() on the hot path. Disable
    // only for services with no latency-sensitive timestamps (e.g. backtester).
    bool calibrate_tsc{true};

    // Path to the host CPU-affinity topology TOML. Empty = no pinning
    // (dev-laptop default). Each service looks up its own role names
    // against this topology at thread construction; unmatched roles
    // fall through as unpinned. See bpt-common/util/topology.h for the
    // file format. Typical deploy: one topology per host class (laptop,
    // prod-host-a, prod-host-b) under deploy/topology/, chosen via env.
    std::string topology_path;
};

// Populate the shared keys of BaseSettings from a TOML root table. Reads:
//   environment = "prod"
//   [aeron].media_driver_dir
//   [logging]  (via bpt::common::logging::from_toml)
//   [metrics].port
// Tolerates any missing block; unset fields keep their defaults. The service
// loader calls this first, then reads its own service-specific TOML keys.
void load_base_settings(const toml::table& root, BaseSettings& base);

}  // namespace bpt::app
