#include "strategy/app/strategy_app.h"
#include "strategy/config/config.h"
#include "strategy/messaging/aeron_bus.h"

#include <bpt_app/app.h>
#include <bpt_app/cli.h>
#include <bpt_common/aeron/chaos_config.h>
#include <bpt_common/env.h>
#include <bpt_common/logging.h>
#include <memory>
#include <string>

namespace {

// Role-qualified service name: "bpt-strat-<variant>" where variant is
// a short code for cfg.strat.strategy.type. Hosts routinely run the
// same strategy binary multiple times (canary alongside live, A/B
// variants, different instrument universes), so the variant code
// makes `top -H` and log files instance-aware.
// Length budget: "bpt-strat-" is 10 chars → up to 5 chars for variant
// fits inside the 15-char comm cap; unknown types fall back to the
// first 5 chars of the type string.
std::string derive_service_name(const std::string& strategy_type) {
    // Strategy `type` strings are PascalCase per config convention
    // (e.g. AvellanedaStoikovStrategy, OFIStrategy). Map to short
    // codes that fit the 15-char comm budget.
    std::string variant;
    if (strategy_type == "AvellanedaStoikovStrategy")
        variant = "as";
    else if (strategy_type == "RegimeSwitchStrategy")
        variant = "rs";
    else if (strategy_type == "OFIStrategy")
        variant = "ofi";
    else if (strategy_type == "HmmStrategy")
        variant = "hmm";
    else if (strategy_type == "MomentumStrategy")
        variant = "mom";
    else if (strategy_type == "FundingArbStrategy")
        variant = "farb";
    else
        variant = strategy_type.substr(0, 5);
    return "bpt-strat-" + variant;
}

}  // namespace

int main(int argc, char* argv[]) {
    auto args = bpt::app::parse_cli(argc, argv, "bpt-strategy", "strategy engine");

    // Bootstrap logger up-front so any pre-run() failure (config load,
    // chaos config) lands in the same sink. bpt::app::run reinits with
    // the loaded LogConfig later. Service name is finalized after the
    // config loads — re-init then so log lines pick up the strategy
    // variant suffix.
    bpt::common::logging::init("bpt-strategy");

    try {
        auto app_cfg = bpt::strategy::config::AppConfig::load(args.config_path);
        const std::string service_name = derive_service_name(app_cfg.strat.strategy.type);
        bpt::common::logging::init(service_name);

        // Optional fault injection (dev/qa only). Must run before
        // bpt::app::run builds the AeronBus — Subscribers consult the
        // registry at ctor time.
        bpt::common::aeron::install_chaos_from_toml(args.config_path,
                                                    bpt::common::to_string(app_cfg.base.environment),
                                                    service_name);

        return bpt::app::run(
            service_name,
            std::move(app_cfg),
            [](auto& cfg, auto& ctx) -> std::unique_ptr<bpt::app::IService> {
                auto bus = bpt::strategy::messaging::StrategyAeronBus::build(ctx.aeron, cfg);
                return std::make_unique<bpt::strategy::StrategyApp>(std::move(cfg), std::move(bus), ctx.topology);
            });
    } catch (const std::exception& e) {
        bpt::common::log::error("Fatal: {}", e.what());
        return 1;
    }
}
