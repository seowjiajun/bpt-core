#include "strategy/app/strategy_app.h"
#include "strategy/config/config.h"

#include <Aeron.h>

#include <CLI/CLI.hpp>
#include <execinfo.h>
#include <string>
#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>
#include <bpt_common/signal.h>
#include <bpt_common/util/tsc_clock.h>

int main(int argc, char* argv[]) {
    bpt::common::signal::install();

    CLI::App app{"bpt-strategy — strategy engine"};
    std::string config_path = "config/fenrir.toml";
    app.add_option("-c,--config", config_path, "Path to TOML config file")
        ->capture_default_str()
        ->check(CLI::ExistingFile);
    CLI11_PARSE(app, argc, argv);

    bpt::strategy::config::AppConfig app_cfg;
    try {
        app_cfg = bpt::strategy::config::AppConfig::load(config_path);
    } catch (const std::exception& e) {
        bpt::common::logging::init("bpt-strategy");
        bpt::common::log::error("{}", e.what());
        return 1;
    }

    bpt::common::logging::init("bpt-strategy", app_cfg.logging);
    bpt::common::util::TscClock::calibrate();

    ::aeron::Context aeron_ctx;
    if (!app_cfg.aeron.media_driver_dir.empty())
        aeron_ctx.aeronDir(app_cfg.aeron.media_driver_dir);
    aeron_ctx.errorHandler([](const std::exception& e) {
        bpt::common::log::error("[Aeron] error handler: {}", e.what());
        void* frames[32];
        int n = ::backtrace(frames, 32);
        char** syms = ::backtrace_symbols(frames, n);
        for (int i = 0; i < n; ++i)
            bpt::common::log::error("  {}", syms ? syms[i] : "???");
        free(syms);
        // do NOT exit — let the poll loop continue
    });
    auto aeron = ::aeron::Aeron::connect(aeron_ctx);
    bpt::common::log::info("Connected to Aeron MediaDriver");

    try {
        bpt::strategy::StrategyApp app(std::move(app_cfg), std::move(aeron));
        app.run();
    } catch (const std::exception& e) {
        bpt::common::log::error("Fatal: {}", e.what());
        return 1;
    }

    return 0;
}
