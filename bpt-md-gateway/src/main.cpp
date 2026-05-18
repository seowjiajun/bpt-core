#include "md_gateway/app/md_gateway_service.h"
#include "md_gateway/config/settings.h"
#include "md_gateway/messaging/aeron_bus.h"

#include <bpt_app/app.h>
#include <bpt_app/cli.h>
#include <bpt_common/aeron/chaos_config.h>
#include <bpt_common/env.h>
#include <bpt_common/logging.h>
#include <bpt_common/util/service_name.h>
#include <memory>
#include <string>

int main(int argc, char* argv[]) {
    auto args = bpt::app::parse_cli(argc, argv, "bpt-md-gateway", "market data aggregator");

    bpt::common::logging::init("bpt-md-gateway");

    try {
        auto cfg = bpt::md_gateway::config::load(args.config_path);
        const std::string service_name = bpt::common::util::derive_service_name("mdgw", cfg.exchanges);
        bpt::common::logging::init(service_name);

        // Optional fault injection (dev/qa only). Must run before
        // bpt::app::run builds the MdGatewayBus — Subscribers consult the
        // registry at ctor time.
        bpt::common::aeron::install_chaos_from_toml(args.config_path,
                                                    bpt::common::to_string(cfg.base.environment),
                                                    service_name);

        return bpt::app::run(service_name,
                             std::move(cfg),
                             [](auto& settings, auto& ctx) -> std::unique_ptr<bpt::app::IService> {
                                 auto bus = bpt::md_gateway::messaging::MdGatewayAeronBus::build(ctx.aeron, settings);
                                 return std::make_unique<bpt::md_gateway::MdGatewayService>(std::move(settings),
                                                                                        std::move(bus.control_sub),
                                                                                        std::move(bus.md_pub),
                                                                                        std::move(bus.ack_pub),
                                                                                        std::move(bus.funding_pub),
                                                                                        std::move(bus.stats_pub),
                                                                                        ctx.topology);
                             });
    } catch (const std::exception& e) {
        bpt::common::log::error("Fatal: {}", e.what());
        return 1;
    }
}
