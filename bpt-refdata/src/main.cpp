#include "refdata/adapter/credentials.h"
#include "refdata/app/refdata_app.h"
#include "refdata/config/settings.h"
#include "refdata/messaging/aeron_bus.h"

#include <algorithm>
#include <bpt_app/app.h>
#include <bpt_app/cli.h>
#include <bpt_common/aeron/chaos_config.h>
#include <bpt_common/env.h>
#include <bpt_common/logging.h>
#include <bpt_common/secrets/secrets_client.h>
#include <cctype>
#include <fmt/format.h>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

// Fetch per-adapter systemd-creds and convert to typed ExchangeCredentials.
// Runs inside the build callable so logging is already initialised by
// bpt::app::run() before we start talking about credential loads.
std::map<std::string, bpt::refdata::adapter::ExchangeCredentials> load_credentials(
    const std::vector<bpt::refdata::config::AdapterConfig>& adapters,
    bpt::common::Env env) {
    const bool strict = (env == bpt::common::Env::QA || env == bpt::common::Env::PROD);
    std::map<std::string, bpt::refdata::adapter::ExchangeCredentials> creds;
    for (const auto& a_cfg : adapters) {
        // Disabled adapters won't run, so don't try to load their credentials —
        // keeps a half-configured disabled entry from blowing up startup.
        if (!a_cfg.enabled)
            continue;

        if (a_cfg.secret_name.empty()) {
            // Most refdata pulls are public endpoints, but in qa/prod a missing
            // secret_name for an enabled adapter is more likely a typo than
            // intentional — refuse to start so it surfaces at deploy time.
            if (strict)
                throw std::runtime_error(fmt::format("env={} but adapter {} has empty secret_name — refusing to start",
                                                     bpt::common::to_string(env),
                                                     a_cfg.exchange));
            bpt::common::log::warn("No secret_name for {} — adapter will have empty credentials (dev only)",
                                   a_cfg.exchange);
            creds[a_cfg.exchange] = {};
            continue;
        }
        const auto kv = bpt::common::secrets::fetch(a_cfg.secret_name, env);
        creds[a_cfg.exchange] = bpt::refdata::adapter::credentials_from_secret(a_cfg.exchange, kv);
        bpt::common::log::info("Loaded credentials for {}", a_cfg.exchange);
    }
    return creds;
}

// Role-qualified service name: "bpt-rfd-<venue>" when exactly one
// adapter is enabled, else generic "bpt-rfd". Refdata typically runs
// one process per venue so the single-adapter case is the default
// deploy shape. Compact "rfd" form saves budget for long venue names
// — "bpt-rfd-hyperliqu" fits 15 (shows 8 venue chars) where the
// verbose "bpt-refdata-hyp" only shows 3.
std::string derive_service_name(const std::vector<bpt::refdata::config::AdapterConfig>& adapters) {
    std::vector<std::string> enabled;
    for (const auto& a : adapters) {
        if (a.enabled)
            enabled.push_back(a.exchange);
    }
    std::string name = "bpt-rfd";
    if (enabled.size() == 1) {
        std::string venue = enabled[0];
        std::transform(venue.begin(), venue.end(), venue.begin(), [](unsigned char c) { return std::tolower(c); });
        name += "-" + venue;
    }
    return name;
}

}  // namespace

int main(int argc, char** argv) {
    auto args = bpt::app::parse_cli(argc, argv, "bpt-refdata", "instrument reference data service");

    bpt::common::logging::init("bpt-refdata");

    try {
        auto settings = bpt::refdata::config::load(args.config_path);
        const std::string service_name = derive_service_name(settings.adapters);
        bpt::common::logging::init(service_name);

        // Optional fault injection (dev/qa only). Must run before
        // bpt::app::run builds the AeronBus — Subscribers consult the
        // registry at ctor time.
        bpt::common::aeron::install_chaos_from_toml(args.config_path,
                                                    bpt::common::to_string(settings.base.environment),
                                                    service_name);

        return bpt::app::run(service_name,
                             std::move(settings),
                             [](auto& cfg, auto& ctx) -> std::unique_ptr<bpt::app::IService> {
                                 auto creds = load_credentials(cfg.adapters, cfg.base.environment);

                                 // Composition root: build the Aeron-backed bus adapters
                                 // as one unit, then hand the ports to RefdataApp.
                                 auto bus = bpt::refdata::messaging::AeronBus::build(ctx.aeron, cfg);

                                 return std::make_unique<bpt::refdata::RefdataApp>(std::move(cfg),
                                                                                   std::move(bus.control_source),
                                                                                   std::move(bus.snapshot_sink),
                                                                                   std::move(bus.delta_sink),
                                                                                   std::move(bus.fee_sink),
                                                                                   std::move(bus.status_sink),
                                                                                   std::move(creds));
                             });
    } catch (const std::exception& e) {
        bpt::common::log::error("Fatal: {}", e.what());
        return 1;
    }
}
