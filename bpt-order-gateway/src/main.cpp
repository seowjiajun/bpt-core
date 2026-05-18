// bpt-order-gateway — order routing + risk enforcement.

#include "order_gateway/adapter/common/credentials.h"
#include "order_gateway/app/order_gateway_service.h"
#include "order_gateway/config/settings.h"
#include "order_gateway/messaging/aeron_bus.h"

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
#include <sys/prctl.h>

namespace {

// Load per-adapter systemd-creds and convert to typed ExchangeCredentials.
// Invoked from inside the bpt::app::run() build callable so logging is
// already initialised by the time we report per-adapter load status.
// Strict-mode policy lives in bpt::common::secrets::load_adapter_credential;
// the backtest-only wallet_address override stays here because it's
// order-gateway-specific (refdata's AdapterConfig has no such field).
std::map<std::string, bpt::order_gateway::adapter::ExchangeCredentials> load_credentials(
    const std::vector<bpt::order_gateway::config::AdapterConfig>& adapters,
    bpt::common::Env env) {
    std::map<std::string, bpt::order_gateway::adapter::ExchangeCredentials> creds;
    for (const auto& a_cfg : adapters) {
        auto c = bpt::common::secrets::load_adapter_credential<bpt::order_gateway::adapter::ExchangeCredentials>(
            a_cfg.exchange,
            a_cfg.secret_name,
            env,
            &bpt::order_gateway::adapter::credentials_from_secret);
        // Backtest-only: a wallet_address set directly in AdapterConfig
        // overrides what we got (or didn't get) from secrets. Lets HL
        // backtests pretend a wallet is configured without populating
        // a real secret — the backtester accepts any address.
        if (!a_cfg.wallet_address.empty())
            c.wallet_address = a_cfg.wallet_address;
        creds[a_cfg.exchange] = std::move(c);
    }
    return creds;
}

}  // namespace

int main(int argc, char* argv[]) {
    // Suppress core dumps to protect key material (Hyperliquid private key).
    // Has to happen before any child threads could fork — do it first.
    ::prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);

    auto args = bpt::app::parse_cli(argc, argv, "bpt-order-gateway", "order routing + risk enforcement");

    // Bootstrap logger so pre-run failures land in the same sink.
    // Re-initialised below once the venue-suffixed name is derived.
    // bpt::app::run reinits a third time with the loaded LogConfig.
    bpt::common::logging::init("order-gateway");

    try {
        auto cfg = bpt::order_gateway::config::load(args.config_path);
        const std::string service_name = bpt::common::util::derive_service_name("ogw", cfg.exchanges);
        bpt::common::logging::init(service_name);

        // Optional fault injection (dev/qa only). Must run before
        // bpt::app::run builds the OrderGatewayBus — Subscribers consult the
        // registry at ctor time.
        bpt::common::aeron::install_chaos_from_toml(args.config_path,
                                                    bpt::common::to_string(cfg.base.environment),
                                                    service_name);

        return bpt::app::run(service_name,
                             std::move(cfg),
                             [](auto& settings, auto& ctx) -> std::unique_ptr<bpt::app::IService> {
                                 auto creds = load_credentials(settings.gateway.adapters, settings.base.environment);
                                 auto bus = bpt::order_gateway::messaging::OrderGatewayAeronBus::build(ctx.aeron, settings);
                                 return std::make_unique<bpt::order_gateway::OrderGatewayService>(
                                     std::move(settings),
                                     std::move(bus.control_sub),
                                     std::move(bus.exec_pub),
                                     std::move(bus.account_snapshot_pub),
                                     std::move(bus.heartbeat_pub),
                                     std::move(creds),
                                     ctx.topology);
                             });
    } catch (const std::exception& e) {
        bpt::common::log::error("Fatal: {}", e.what());
        return 1;
    }
}
