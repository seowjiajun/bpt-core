/// \file
/// \brief bpt-tape entry point — captures venue WS frames + REST bodies
///        to disk via Tape.
///
/// Imports bpt-md-gateway's adapter library and substitutes recording
/// subclasses that tee raw bytes through bpt::common::recorder::Tape.
/// The mdgw and refdata service binaries are unchanged — this process
/// owns the recording feature in full. No Aeron data publication; a
/// NoopMdPublisher fills the adapter's Pub template slot so parsing
/// still runs identically to live trading.
///
/// All wiring lives in bpt::tape::app::RecorderService — main.cpp is
/// just the CLI front and the bpt::app::run handoff.

#include "tape/app/recorder_service.h"
#include "tape/config/settings.h"

#include <bpt_app/app.h>
#include <bpt_app/cli.h>
#include <bpt_common/logging.h>
#include <exception>
#include <memory>
#include <utility>

int main(int argc, char* argv[]) {
    auto args = bpt::app::parse_cli(argc, argv, "bpt-tape", "venue WS-frame recorder");

    // Bootstrap logger so pre-run failures (config load, etc.) land in
    // the same sink as later lines. bpt::app::run reinitializes with
    // the loaded LogConfig — Quill's create_or_get_logger is idempotent.
    bpt::common::logging::init("bpt-tape");

    try {
        auto cfg = bpt::tape::config::load(args.config_path);
        // connect_aeron=false: the recording host runs zero Aeron
        // infrastructure (no MediaDriver, no SBE publish, no refdata
        // subscription — the universe loads directly from JSON).
        return bpt::app::run(
            "bpt-tape",
            std::move(cfg),
            [](auto& settings, auto& ctx) -> std::unique_ptr<bpt::app::IService> {
                return std::make_unique<bpt::tape::app::RecorderService>(std::move(settings), ctx.topology);
            },
            bpt::app::RunOptions{.connect_aeron = false});
    } catch (const std::exception& e) {
        bpt::common::log::error("Fatal: {}", e.what());
        return 1;
    }
}
