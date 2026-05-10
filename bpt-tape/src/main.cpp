/// bpt-tape — "the tape": captures venue WS frames + REST refdata bodies
/// to disk by importing bpt-md-gateway's adapter library and substituting
/// recording subclasses that tee raw bytes via bpt::common::recorder::RawSpool.
///
/// mdgw and refdata service binaries are unchanged; this process owns the
/// recording feature in full. No Aeron data publication; a no-op publisher
/// satisfies the adapter's Pub template parameter so parsing still runs
/// (cheap on the recording host) and the wire pipeline behaves identically
/// to live trading at every layer except the disk tap.
///
/// All wiring lives in bpt::tape::app::RecorderService — main.cpp is just
/// the entry point.

#include "tape/app/recorder_service.h"
#include "tape/config/settings.h"

#include <exception>
#include <memory>
#include <utility>
#include <bpt_app/app.h>
#include <bpt_app/cli.h>
#include <bpt_common/logging.h>

int main(int argc, char* argv[]) {
    auto args = bpt::app::parse_cli(argc, argv,
                                    "bpt-tape",
                                    "venue WS-frame recorder");

    // Bootstrap logger so config::load failures land in the same sink
    // any later log line would. bpt::app::run reinitializes with the
    // loaded LogConfig later — Quill's create_or_get_logger is
    // idempotent on the same name.
    bpt::common::logging::init("bpt-tape");

    try {
        auto cfg = bpt::tape::config::load(args.config_path);
        // Recording host runs no Aeron consumers — venue WS frames go
        // straight to disk via RawSpool, no SBE published, no refdata
        // catalog subscription (universe loaded directly from JSON).
        // Skip the MediaDriver connect; the recording host needs zero
        // Aeron infrastructure.
        return bpt::app::run("bpt-tape", std::move(cfg),
            [](auto& settings, auto& ctx) -> std::unique_ptr<bpt::app::IService> {
                return std::make_unique<bpt::tape::app::RecorderService>(
                    std::move(settings), ctx.topology);
            },
            bpt::app::RunOptions{.connect_aeron = false});
    } catch (const std::exception& e) {
        bpt::common::log::error("Fatal: {}", e.what());
        return 1;
    }
}
