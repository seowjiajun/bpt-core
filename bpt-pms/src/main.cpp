// bpt-pms — consolidated multi-venue balance / position / PnL state.
// Read-only: queries each configured exchange's account endpoints,
// publishes normalized snapshots on Aeron. Owns nothing that places
// orders — if this process dies, trading doesn't stop.

#include "pms/app/pms_service.h"
#include "pms/config/settings.h"
#include "pms/messaging/aeron_bus.h"

#include <bpt_app/app.h>
#include <bpt_app/cli.h>
#include <bpt_common/aeron/chaos_config.h>
#include <bpt_common/env.h>
#include <bpt_common/logging.h>
#include <memory>
#include <string>

int main(int argc, char** argv) {
    auto args = bpt::app::parse_cli(argc, argv, "bpt-pms", "multi-venue balance / position aggregator");

    bpt::common::logging::init("bpt-pms");

    try {
        auto settings = bpt::pms::config::load(args.config_path);

        // Optional fault injection (dev/qa only). Must run before
        // bpt::app::run builds the AeronBus — Subscribers consult the
        // registry at ctor time.
        bpt::common::aeron::install_chaos_from_toml(args.config_path,
                                                    bpt::common::to_string(settings.base.environment),
                                                    "bpt-pms");

        return bpt::app::run("bpt-pms",
                             std::move(settings),
                             [](auto& cfg, auto& ctx) -> std::unique_ptr<bpt::app::IService> {
                                 auto bus = bpt::pms::messaging::PmsAeronBus::build(ctx.aeron, cfg);
                                 return std::make_unique<bpt::pms::PmsService>(std::move(cfg), std::move(bus));
                             });
    } catch (const std::exception& e) {
        bpt::common::log::error("Fatal: {}", e.what());
        return 1;
    }
}
