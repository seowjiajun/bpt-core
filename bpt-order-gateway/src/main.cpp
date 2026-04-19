// bpt-order-gateway — order routing + risk enforcement.

#include "order_gateway/adapter/common/credentials.h"
#include "order_gateway/app/gateway_app.h"
#include "order_gateway/config/settings.h"

#include <CLI/CLI.hpp>
#include <map>
#include <memory>
#include <string>
#include <sys/prctl.h>
#include <bpt_app/app.h>
#include <bpt_common/logging.h>
#include <bpt_common/secrets/secrets_client.h>

namespace {

// Load per-adapter systemd-creds and convert to typed ExchangeCredentials.
// Invoked from inside the bpt::app::run() build callable so logging is
// already initialised by the time we report per-adapter load status.
std::map<std::string, bpt::order_gateway::adapter::ExchangeCredentials>
load_credentials(const std::vector<bpt::order_gateway::config::AdapterConfig>& adapters) {
    std::map<std::string, bpt::order_gateway::adapter::ExchangeCredentials> creds;
    for (const auto& a_cfg : adapters) {
        if (a_cfg.secret_name.empty()) {
            bpt::common::log::warn(
                "[OrderGateway] No secret_name for {} — adapter will have empty credentials",
                a_cfg.exchange);
            creds[a_cfg.exchange] = {};
            continue;
        }
        const auto kv = bpt::common::secrets::fetch(a_cfg.secret_name);
        creds[a_cfg.exchange] = bpt::order_gateway::adapter::credentials_from_secret(a_cfg.exchange, kv);
        bpt::common::log::info("[OrderGateway] Loaded credentials for {}", a_cfg.exchange);
    }
    return creds;
}

}  // namespace

int main(int argc, char* argv[]) {
    // Suppress core dumps to protect key material (Hyperliquid private key).
    // Has to happen before any child threads could fork — do it first.
    ::prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);

    CLI::App cli{"bpt-order-gateway — order routing + risk enforcement"};
    std::string config_path = "config/order-gateway.toml";
    cli.add_option("-c,--config", config_path, "Path to TOML config file")
        ->capture_default_str()
        ->check(CLI::ExistingFile);
    CLI11_PARSE(cli, argc, argv);

    bpt::order_gateway::config::Settings cfg;
    try {
        cfg = bpt::order_gateway::config::load(config_path);
    } catch (const std::exception& e) {
        bpt::common::logging::init("order-gateway");
        bpt::common::log::error("[OrderGateway] Failed to load config: {}", e.what());
        return 1;
    }

    try {
        return bpt::app::run("order-gateway", std::move(cfg),
            [](auto& settings, auto& ctx) -> std::unique_ptr<bpt::app::IService> {
                auto creds = load_credentials(settings.gateway.adapters);
                return std::make_unique<bpt::order_gateway::OrderGatewayApp>(
                    std::move(settings), ctx.aeron, std::move(creds));
            });
    } catch (const std::exception& e) {
        bpt::common::log::error("[OrderGateway] Fatal: {}", e.what());
        return 1;
    }
}
