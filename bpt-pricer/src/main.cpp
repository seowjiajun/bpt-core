#include "pricer/app/pricer_service.h"
#include "pricer/config/settings.h"
#include "pricer/messaging/aeron_bus.h"

#include <bpt_app/app.h>
#include <bpt_app/cli.h>
#include <bpt_common/aeron/chaos_config.h>
#include <bpt_common/env.h>
#include <bpt_common/logging.h>
#include <memory>
#include <string>

int main(int argc, char** argv) {
    auto args = bpt::app::parse_cli(argc, argv, "bpt-pricer", "options Greeks + vol surface pricer");

    bpt::common::logging::init("bpt-pricer");

    try {
        auto settings = bpt::pricer::config::load(args.config_path);

        // Optional fault injection (dev/qa only). Must run before
        // bpt::app::run builds the AeronBus — Subscribers consult the
        // registry at ctor time.
        bpt::common::aeron::install_chaos_from_toml(args.config_path,
                                                    bpt::common::to_string(settings.base.environment),
                                                    "bpt-pricer");

        return bpt::app::run("bpt-pricer",
                             std::move(settings),
                             [](auto& cfg, auto& ctx) -> std::unique_ptr<bpt::app::IService> {
                                 auto bus = bpt::pricer::messaging::PricerAeronBus::build(ctx.aeron, cfg);
                                 return std::make_unique<bpt::pricer::PricerService>(std::move(cfg), std::move(bus));
                             });
    } catch (const std::exception& e) {
        bpt::common::log::error("Fatal: {}", e.what());
        return 1;
    }
}
