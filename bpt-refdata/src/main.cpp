#include "refdata/adapter/credentials.h"
#include "refdata/app/refdata_service.h"
#include "refdata/config/settings.h"
#include "refdata/messaging/aeron_bus.h"

#include <bpt_app/app.h>
#include <bpt_app/cli.h>
#include <bpt_common/aeron/chaos_config.h>
#include <bpt_common/env.h>
#include <bpt_common/logging.h>
#include <bpt_common/secrets/load_adapter_credential.h>
#include <bpt_common/util/service_name.h>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

// Fetch per-adapter systemd-creds and convert to typed ExchangeCredentials.
// Runs inside the build callable so logging is already initialised by
// bpt::app::run() before we start talking about credential loads.
// Strict-mode policy lives in bpt::common::secrets::load_adapter_credential;
// the `.enabled` filter stays here because order-gateway's AdapterConfig
// has no such field — keeping the iteration local makes the difference
// obvious.
std::map<std::string, bpt::refdata::adapter::ExchangeCredentials> load_credentials(
    const std::vector<bpt::refdata::config::AdapterConfig>& adapters,
    bpt::common::Env env) {
    std::map<std::string, bpt::refdata::adapter::ExchangeCredentials> creds;
    for (const auto& a_cfg : adapters) {
        // Disabled adapters won't run, so don't try to load their credentials —
        // keeps a half-configured disabled entry from blowing up startup.
        if (!a_cfg.enabled)
            continue;
        creds[a_cfg.exchange] =
            bpt::common::secrets::load_adapter_credential<bpt::refdata::adapter::ExchangeCredentials>(
                a_cfg.exchange,
                a_cfg.secret_name,
                env,
                &bpt::refdata::adapter::credentials_from_secret);
    }
    return creds;
}

// Collect the `.exchange` of each enabled adapter — used to feed the
// shared bpt::common::util::derive_service_name() helper, which then
// emits "bpt-rfd-<venue>" when exactly one venue is active.
std::vector<std::string> enabled_venues(const std::vector<bpt::refdata::config::AdapterConfig>& adapters) {
    std::vector<std::string> out;
    for (const auto& a : adapters) {
        if (a.enabled)
            out.push_back(a.exchange);
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    auto args = bpt::app::parse_cli(argc, argv, "bpt-refdata", "instrument reference data service");

    bpt::common::logging::init("bpt-refdata");

    try {
        auto settings = bpt::refdata::config::load(args.config_path);
        const std::string service_name =
            bpt::common::util::derive_service_name("rfd", enabled_venues(settings.adapters));
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
                                 // as one unit, then hand the ports to RefdataService.
                                 auto bus = bpt::refdata::messaging::AeronBus::build(ctx.aeron, cfg);

                                 return std::make_unique<bpt::refdata::RefdataService>(std::move(cfg),
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
