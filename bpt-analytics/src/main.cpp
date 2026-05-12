#include "analytics/app/tyr_app.h"
#include "analytics/config/settings.h"
#include "analytics/messaging/aeron_bus.h"

#include <bpt_app/app.h>
#include <bpt_app/cli.h>
#include <bpt_common/aeron/chaos_config.h>
#include <bpt_common/env.h>
#include <bpt_common/logging.h>
#include <memory>
#include <string>

int main(int argc, char** argv) {
    auto args = bpt::app::parse_cli(argc, argv, "bpt-analytics", "markouts, toxicity, fill-rate analytics");

    bpt::common::logging::init("bpt-analytics");

    try {
        auto settings = bpt::analytics::config::load(args.config_path);

        // Optional fault injection (dev/qa only). Must run before
        // bpt::app::run builds the AeronBus — Subscribers consult the
        // registry at ctor time.
        bpt::common::aeron::install_chaos_from_toml(args.config_path,
                                                    bpt::common::to_string(settings.base.environment),
                                                    "bpt-analytics");

        return bpt::app::run("bpt-analytics",
                             std::move(settings),
                             [](auto& cfg, auto& ctx) -> std::unique_ptr<bpt::app::IService> {
                                 auto bus = bpt::analytics::messaging::AnalyticsAeronBus::build(ctx.aeron, cfg);
                                 return std::make_unique<bpt::analytics::AnalyticsApp>(std::move(cfg), std::move(bus));
                             });
    } catch (const std::exception& e) {
        bpt::common::log::error("Fatal: {}", e.what());
        return 1;
    }
}
