#include "md_gateway/app/md_gateway_app.h"
#include "md_gateway/config/settings.h"

#include <CLI/CLI.hpp>
#include <algorithm>
#include <cctype>
#include <memory>
#include <string>
#include <bpt_app/app.h>
#include <bpt_common/logging.h>

namespace {

// Role-qualified service name: "bpt-mdgw-<venue>" when exactly one
// exchange is active, else the generic "bpt-mdgw". Feeds comm
// (via bpt::app::run → prctl), log filename, [logger] prefix, and
// quill backend thread name — one identity string across all four.
// Matches the "bpt-ogw" compact form used by bpt-order-gateway so
// operators see a consistent abbreviation pattern across services.
std::string derive_service_name(const std::vector<std::string>& exchanges) {
    std::string name = "bpt-mdgw";
    if (exchanges.size() == 1) {
        std::string venue = exchanges[0];
        std::transform(venue.begin(), venue.end(), venue.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        name += "-" + venue;
    }
    return name;
}

}  // namespace

int main(int argc, char* argv[]) {
    CLI::App cli{"bpt-md-gateway — market data aggregator"};
    std::string config_path;
    cli.add_option("-c,--config", config_path, "Path to TOML config file")
        ->required()
        ->check(CLI::ExistingFile);
    CLI11_PARSE(cli, argc, argv);

    bpt::md_gateway::config::Settings cfg;
    try {
        cfg = bpt::md_gateway::config::load(config_path);
    } catch (const std::exception& e) {
        bpt::common::logging::init("bpt-md-gateway");
        bpt::common::log::error("Failed to load config: {}", e.what());
        return 1;
    }

    const std::string service_name = derive_service_name(cfg.exchanges);

    try {
        return bpt::app::run(service_name, std::move(cfg),
            [](auto& settings, auto& ctx) -> std::unique_ptr<bpt::app::IService> {
                return std::make_unique<bpt::md_gateway::MdGatewayApp>(
                    std::move(settings), ctx.aeron, ctx.topology);
            });
    } catch (const std::exception& e) {
        bpt::common::log::error("Fatal: {}", e.what());
        return 1;
    }
}
