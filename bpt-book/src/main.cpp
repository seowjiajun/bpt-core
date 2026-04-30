// bpt-book — consolidated multi-venue balance / position / PnL state.
// Read-only: queries each configured exchange's account endpoints,
// publishes normalized snapshots on Aeron. Owns nothing that places
// orders — if this process dies, trading doesn't stop.

#include "book/app/book_app.h"
#include "book/config/settings.h"
#include "book/messaging/aeron_bus.h"

#include <CLI/CLI.hpp>
#include <memory>
#include <string>
#include <bpt_app/app.h>
#include <bpt_common/logging.h>

int main(int argc, char** argv) {
    CLI::App cli{"bpt-book — multi-venue balance / position aggregator"};
    std::string config_path;
    cli.add_option("-c,--config", config_path, "Path to TOML config file")
        ->required()
        ->check(CLI::ExistingFile);
    CLI11_PARSE(cli, argc, argv);

    bpt::book::config::Settings settings;
    try {
        settings = bpt::book::config::load(config_path);
    } catch (const std::exception& e) {
        bpt::common::logging::init("bpt-book");
        bpt::common::log::error("Failed to load config: {}", e.what());
        return 1;
    }

    try {
        return bpt::app::run("bpt-book", std::move(settings),
            [](auto& cfg, auto& ctx) -> std::unique_ptr<bpt::app::IService> {
                auto bus = bpt::book::messaging::BookAeronBus::build(ctx.aeron, cfg);
                return std::make_unique<bpt::book::BookApp>(std::move(cfg), std::move(bus));
            });
    } catch (const std::exception& e) {
        bpt::common::log::error("Fatal: {}", e.what());
        return 1;
    }
}
