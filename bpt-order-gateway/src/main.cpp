// bpt-order-gateway — order routing + risk enforcement.

#include "order_gateway/adapter/common/credentials.h"
#include "order_gateway/app/gateway_app.h"
#include "order_gateway/config/settings.h"

#include <CLI/CLI.hpp>
#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/prctl.h>
#include <bpt_app/app.h>
#include <bpt_common/logging.h>
#include <bpt_common/secrets/secrets_client.h>
#include <fmt/format.h>

namespace {

// Load per-adapter systemd-creds and convert to typed ExchangeCredentials.
// Invoked from inside the bpt::app::run() build callable so logging is
// already initialised by the time we report per-adapter load status.
std::map<std::string, bpt::order_gateway::adapter::ExchangeCredentials>
load_credentials(const std::vector<bpt::order_gateway::config::AdapterConfig>& adapters,
                 bpt::common::Env env) {
    const bool strict = (env == bpt::common::Env::QA || env == bpt::common::Env::PROD);
    std::map<std::string, bpt::order_gateway::adapter::ExchangeCredentials> creds;
    for (const auto& a_cfg : adapters) {
        if (a_cfg.secret_name.empty()) {
            // An order-gateway adapter without credentials would fail on the
            // first authed request anyway; in qa/prod this is a deployment
            // bug, refuse to start so it's caught before any live trading.
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
        creds[a_cfg.exchange] = bpt::order_gateway::adapter::credentials_from_secret(a_cfg.exchange, kv);
        bpt::common::log::info("Loaded credentials for {}", a_cfg.exchange);
    }
    return creds;
}

// Role-qualified service name: "bpt-ogw-<venue>" when exactly one
// exchange is active, else the generic "bpt-ogw". Feeds comm (via
// bpt::app::run → prctl), log filename, [logger] prefix, and quill
// backend thread name — one identity string across all four. Mirrors
// the md-gateway derivation so operators can grep consistently.
std::string derive_service_name(const std::vector<std::string>& exchanges) {
    std::string name = "bpt-ogw";
    if (exchanges.size() == 1) {
        std::string venue = exchanges[0];
        std::transform(venue.begin(), venue.end(), venue.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        name += "-" + venue;
    }
    return name;
}

}  // namespace

int main(int argc, char* argv[]) {
    // Suppress core dumps to protect key material (Hyperliquid private key).
    // Has to happen before any child threads could fork — do it first.
    ::prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);

    CLI::App cli{"bpt-order-gateway — order routing + risk enforcement"};
    std::string config_path;
    cli.add_option("-c,--config", config_path, "Path to TOML config file")
        ->required()
        ->check(CLI::ExistingFile);
    CLI11_PARSE(cli, argc, argv);

    bpt::order_gateway::config::Settings cfg;
    try {
        cfg = bpt::order_gateway::config::load(config_path);
    } catch (const std::exception& e) {
        bpt::common::logging::init("order-gateway");
        bpt::common::log::error("Failed to load config: {}", e.what());
        return 1;
    }

    const std::string service_name = derive_service_name(cfg.exchanges);

    try {
        return bpt::app::run(service_name, std::move(cfg),
            [](auto& settings, auto& ctx) -> std::unique_ptr<bpt::app::IService> {
                auto creds = load_credentials(settings.gateway.adapters, settings.base.environment);
                return std::make_unique<bpt::order_gateway::OrderGatewayApp>(
                    std::move(settings), ctx.aeron, std::move(creds), ctx.topology);
            });
    } catch (const std::exception& e) {
        bpt::common::log::error("Fatal: {}", e.what());
        return 1;
    }
}
