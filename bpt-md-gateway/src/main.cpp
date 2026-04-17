#include "md_gateway/app/bpt-md-gateway_app.h"
#include "md_gateway/config/settings.h"

#include <chrono>
#include <string>
#include <yggdrasil/aeron/aeron_utils.h>
#include <yggdrasil/logging.h>
#include <yggdrasil/signal.h>
#include <yggdrasil/util/tsc_clock.h>

int main(int argc, char* argv[]) {
    ygg::signal::install();

    const std::string config_path = (argc > 1) ? argv[1] : "config/bpt-md-gateway.toml";

    bpt::md_gateway::config::Settings cfg;
    try {
        cfg = bpt::md_gateway::config::load(config_path);
    } catch (const std::exception& e) {
        ygg::logging::init("bpt-md-gateway");
        ygg::log::error("Failed to load config: {}", e.what());
        return 1;
    }

    ygg::logging::init("bpt-md-gateway", cfg.logging);
    ygg::util::TscClock::calibrate();

    auto aeron = ygg::aeron::connect(cfg.aeron.media_driver_dir);
    ygg::log::info("MdGateway connected to Aeron MediaDriver");

    try {
        bpt::md_gateway::MdGatewayApp app(std::move(cfg), std::move(aeron));
        app.run();
    } catch (const std::exception& e) {
        ygg::log::error("Fatal: {}", e.what());
        return 1;
    }

    return 0;
}
