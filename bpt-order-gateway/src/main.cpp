// bpt-order-gateway — Order Gateway service

#include "order_gateway/adapter/common/credentials.h"
#include "order_gateway/app/gateway_app.h"
#include "order_gateway/config/settings.h"

#include <CLI/CLI.hpp>
#include <map>
#include <string>
#include <sys/prctl.h>
#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>
#include <bpt_common/secrets/secrets_client.h>
#include <bpt_common/signal.h>

int main(int argc, char* argv[]) {
    // Suppress core dumps to protect key material (Hyperliquid private key)
    ::prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);

    bpt::common::signal::install();

    CLI::App app{"bpt-order-gateway — order routing + risk enforcement"};
    std::string config_path = "config/order-gateway.toml";
    app.add_option("-c,--config", config_path, "Path to TOML config file")
        ->capture_default_str()
        ->check(CLI::ExistingFile);
    CLI11_PARSE(app, argc, argv);

    bpt::order_gateway::config::Settings cfg;
    try {
        cfg = bpt::order_gateway::config::load(config_path);
    } catch (const std::exception& e) {
        bpt::common::logging::init("order-gateway");
        bpt::common::log::error("[OrderGateway] Failed to load config: {}", e.what());
        return 1;
    }

    bpt::common::logging::init("order-gateway", cfg.logging);
    bpt::common::log::info("[OrderGateway] Starting");

    std::map<std::string, bpt::order_gateway::adapter::ExchangeCredentials> creds;
    for (const auto& a_cfg : cfg.gateway.adapters) {
        if (a_cfg.secret_name.empty()) {
            bpt::common::log::warn("[OrderGateway] No secret_name for {} — adapter will have empty credentials", a_cfg.exchange);
            creds[a_cfg.exchange] = {};
            continue;
        }
        try {
            const auto kv = bpt::common::secrets::fetch(a_cfg.secret_name);
            creds[a_cfg.exchange] = bpt::order_gateway::adapter::credentials_from_secret(a_cfg.exchange, kv);
            bpt::common::log::info("[OrderGateway] Loaded credentials for {}", a_cfg.exchange);
        } catch (const std::exception& e) {
            bpt::common::log::error("[OrderGateway] Failed to load credentials for {}: {}", a_cfg.exchange, e.what());
            return 1;
        }
    }

    std::shared_ptr<aeron::Aeron> aeron;
    try {
        aeron = bpt::common::aeron::connect(cfg.aeron.media_driver_dir);
        bpt::common::log::info("[OrderGateway] Connected to Aeron MediaDriver");
    } catch (const std::exception& e) {
        bpt::common::log::error("[OrderGateway] Failed to connect to Aeron: {}", e.what());
        return 1;
    }

    try {
        bpt::order_gateway::OrderGatewayApp app(std::move(cfg), std::move(aeron), std::move(creds));
        app.run();
    } catch (const std::exception& e) {
        bpt::common::log::error("[OrderGateway] Fatal: {}", e.what());
        return 1;
    }

    return 0;
}
