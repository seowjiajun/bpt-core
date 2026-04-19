#include "refdata/adapter/credentials.h"
#include "refdata/app/refdata_app.h"
#include "refdata/config/settings.h"

#include <CLI/CLI.hpp>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <bpt_app/app.h>
#include <bpt_common/logging.h>
#include <bpt_common/secrets/secrets_client.h>

namespace {

// Fetch per-adapter systemd-creds and convert to typed ExchangeCredentials.
// Runs inside the build callable so logging is already initialised by
// bpt::app::run() before we start talking about credential loads.
std::map<std::string, bpt::refdata::adapter::ExchangeCredentials>
load_credentials(const std::vector<bpt::refdata::config::AdapterConfig>& adapters) {
    std::map<std::string, bpt::refdata::adapter::ExchangeCredentials> creds;
    for (const auto& a_cfg : adapters) {
        if (a_cfg.secret_name.empty()) {
            bpt::common::log::warn(
                "[Refdata] No secret_name for {} — adapter will have empty credentials",
                a_cfg.exchange);
            creds[a_cfg.exchange] = {};
            continue;
        }
        const auto kv = bpt::common::secrets::fetch(a_cfg.secret_name);
        creds[a_cfg.exchange] = bpt::refdata::adapter::credentials_from_secret(a_cfg.exchange, kv);
        bpt::common::log::info("[Refdata] Loaded credentials for {}", a_cfg.exchange);
    }
    return creds;
}

}  // namespace

int main(int argc, char** argv) {
    CLI::App cli{"bpt-refdata — instrument reference data service"};
    std::string config_path = "config/bpt-refdata.toml";
    cli.add_option("-c,--config", config_path, "Path to TOML config file")
        ->capture_default_str()
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
                auto creds = load_credentials(cfg.adapters);
                return std::make_unique<bpt::refdata::RefdataApp>(
                    std::move(cfg), ctx.aeron, std::move(creds));
            });
    } catch (const std::exception& e) {
        bpt::common::log::error("Fatal: {}", e.what());
        return 1;
    }
}
