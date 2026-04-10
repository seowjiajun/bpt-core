// Jormungandr — Backtest Exchange Simulator
// The world serpent that swallows the entire market history.

#include <spdlog/spdlog.h>
#include <yggdrasil/aeron/aeron_utils.h>
#include <yggdrasil/logging.h>
#include <yggdrasil/signal.h>

#include <string>

#include "jormungandr/app/jormungandr_app.h"
#include "jormungandr/config/settings.h"

int main(int argc, char* argv[]) {
    ygg::signal::install();

    const std::string config_path = (argc > 1) ? argv[1] : "config/jormungandr.toml";

    jormungandr::config::Settings settings;
    try {
        settings = jormungandr::config::load(config_path);
    } catch (const std::exception& e) {
        ygg::logging::init("jormungandr");
        spdlog::error("[Jormungandr] Failed to load config: {}", e.what());
        return 1;
    }

    ygg::logging::init("jormungandr", settings.logging.dir,
                       ygg::logging::level_from_string(settings.logging.level));
    spdlog::info("[Jormungandr] Starting — the world serpent awakens.");

    std::shared_ptr<aeron::Aeron> aeron;
    try {
        aeron = ygg::aeron::connect(settings.aeron.media_driver_dir);
        spdlog::info("[Jormungandr] Connected to Aeron MediaDriver");
    } catch (const std::exception& e) {
        spdlog::error("[Jormungandr] Failed to connect to Aeron: {}", e.what());
        return 1;
    }

    try {
        jormungandr::JormungandrApp app(std::move(settings), std::move(aeron));
        app.run();
    } catch (const std::exception& e) {
        spdlog::error("[Jormungandr] Fatal: {}", e.what());
        return 1;
    }

    return 0;
}
