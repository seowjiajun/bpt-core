#include "refdata/adapter/credentials.h"
#include "refdata/app/refdata_app.h"
#include "refdata/config/settings.h"

#include <CLI/CLI.hpp>
#include <chrono>
#include <map>
#include <string>
#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>
#include <bpt_common/secrets/secrets_client.h>
#include <bpt_common/signal.h>
#include <bpt_common/util/tsc_clock.h>

int main(int argc, char** argv) {
    bpt::common::signal::install();

    CLI::App app{"bpt-refdata — instrument reference data service"};
    std::string config_path = "config/bpt-refdata.toml";
    app.add_option("-c,--config", config_path, "Path to TOML config file")
        ->capture_default_str()
        ->check(CLI::ExistingFile);
    CLI11_PARSE(app, argc, argv);

    bpt::refdata::config::Settings settings;
    try {
        settings = bpt::refdata::config::load(config_path);
    } catch (const std::exception& e) {
        bpt::common::logging::init("bpt-refdata");
        bpt::common::log::error("Failed to load config: {}", e.what());
        return 1;
    }

    bpt::common::logging::init("bpt-refdata", settings.logging);
    bpt::common::util::TscClock::calibrate();
    bpt::common::log::info("Starting Refdata Reference Data Service...");

    std::map<std::string, bpt::refdata::adapter::ExchangeCredentials> creds;
    for (const auto& a_cfg : settings.adapters) {
        if (a_cfg.secret_name.empty()) {
            bpt::common::log::warn("[Refdata] No secret_name for {} — adapter will have empty credentials", a_cfg.exchange);
            creds[a_cfg.exchange] = {};
            continue;
        }
        try {
            const auto kv = bpt::common::secrets::fetch(a_cfg.secret_name);
            creds[a_cfg.exchange] = bpt::refdata::adapter::credentials_from_secret(a_cfg.exchange, kv);
            bpt::common::log::info("[Refdata] Loaded credentials for {}", a_cfg.exchange);
        } catch (const std::exception& e) {
            bpt::common::log::error("[Refdata] Failed to load credentials for {}: {}", a_cfg.exchange, e.what());
            return 1;
        }
    }

    auto aeron = bpt::common::aeron::connect(settings.media_driver_dir);

    try {
        bpt::refdata::RefdataApp app(std::move(settings), std::move(aeron), std::move(creds));
        app.run();
    } catch (const std::exception& e) {
        bpt::common::log::error("Fatal: {}", e.what());
        return 1;
    }

    return 0;
}
