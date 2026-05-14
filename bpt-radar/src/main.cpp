#include "radar/app/radar_service.h"
#include "radar/config/settings.h"
#include "radar/messaging/aeron_bus.h"

#include <bpt_app/app.h>
#include <bpt_app/cli.h>
#include <bpt_common/logging.h>
#include <memory>

int main(int argc, char* argv[]) {
    auto args = bpt::app::parse_cli(argc, argv, "bpt-radar", "options market-color aggregator");

    bpt::common::logging::init("bpt-radar");

    try {
        auto cfg = bpt::radar::config::load(args.config_path);
        bpt::common::logging::init("bpt-radar");

        return bpt::app::run(
            "bpt-radar",
            std::move(cfg),
            [](auto& settings, auto& ctx) -> std::unique_ptr<bpt::app::IService> {
                auto bus = bpt::radar::messaging::RadarAeronBus::build(ctx.aeron, settings);
                return std::make_unique<bpt::radar::RadarService>(std::move(settings), std::move(bus));
            });
    } catch (const std::exception& e) {
        bpt::common::log::error("Fatal: {}", e.what());
        return 1;
    }
}
