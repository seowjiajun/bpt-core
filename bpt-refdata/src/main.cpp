#include "refdata/adapter/credentials.h"
#include "refdata/app/refdata_app.h"
#include "refdata/config/settings.h"

#include <CLI/CLI.hpp>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <bpt_app/app.h>
#include <bpt_common/logging.h>
#include <bpt_common/secrets/secrets_client.h>
#include <fmt/format.h>

namespace {

// Fetch per-adapter systemd-creds and convert to typed ExchangeCredentials.
// Runs inside the build callable so logging is already initialised by
// bpt::app::run() before we start talking about credential loads.
std::map<std::string, bpt::refdata::adapter::ExchangeCredentials>
load_credentials(const std::vector<bpt::refdata::config::AdapterConfig>& adapters,
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
                throw std::runtime_error(fmt::format(
                    "env={} but adapter {} has empty secret_name — refusing to start",
                    bpt::common::to_string(env), a_cfg.exchange));
            bpt::common::log::warn(
                "No secret_name for {} — adapter will have empty credentials (dev only)",
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

}  // namespace

int main(int argc, char** argv) {
    CLI::App cli{"bpt-refdata — instrument reference data service"};
    std::string config_path;
    cli.add_option("-c,--config", config_path, "Path to TOML config file")
        ->required()
        ->check(CLI::ExistingFile);
    CLI11_PARSE(cli, argc, argv);

    bpt::refdata::config::Settings settings;
    try {
        settings = bpt::refdata::config::load(config_path);
    } catch (const std::exception& e) {
        bpt::common::logging::init("bpt-refdata");
        bpt::common::log::error("Failed to load config: {}", e.what());
        return 1;
    }

    try {
        return bpt::app::run("bpt-refdata", std::move(settings),
            [](auto& cfg, auto& ctx) -> std::unique_ptr<bpt::app::IService> {
                auto creds = load_credentials(cfg.adapters, cfg.base.environment);
                return std::make_unique<bpt::refdata::RefdataApp>(
                    std::move(cfg), ctx.aeron, std::move(creds));
            });
    } catch (const std::exception& e) {
        bpt::common::log::error("Fatal: {}", e.what());
        return 1;
    }
}
