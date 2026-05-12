#pragma once

/// \file
/// \brief Shared application lifecycle for every bpt-core service.
///
/// Every service's main() funnels through bpt::app::run<Settings>(...) so
/// signal handling, logging, TSC calibration, optional Aeron connect, and
/// graceful shutdown are identical across binaries.
///
/// Typical use:
/// \code
///     int main(int argc, char** argv) {
///         auto args = bpt::app::parse_cli(argc, argv,
///                                         "my-service",
///                                         "what this service does");
///         auto settings = config::load(args.config_path);
///         return bpt::app::run("my-service", std::move(settings),
///             [](auto& cfg, auto& ctx) -> std::unique_ptr<bpt::app::IService> {
///                 return std::make_unique<MyService>(std::move(cfg), ctx.aeron);
///             });
///     }
/// \endcode
///
/// What bpt::app::run owns: signal install, TSC calibrate, logging init,
/// optional Aeron connect, main-thread loop, graceful shutdown.
///
/// What services own: Settings struct + loader, IService implementation.
/// CLI definition is shared via bpt::app::parse_cli (see bpt_app/cli.h)
/// so all services pick up new common flags (--version, etc.) for free.
///
/// Settings type must expose a `bpt::app::BaseSettings base;` member —
/// the template reads `settings.base` for lifecycle knobs before handing
/// the full settings to the build callable.

#include "bpt_app/base_settings.h"

#include <Aeron.h>

#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>
#include <bpt_common/signal.h>
#include <bpt_common/util/topology.h>
#include <bpt_common/util/tsc_clock.h>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <memory>
#include <string>
#include <string_view>
#include <sys/prctl.h>
#include <utility>

namespace bpt::app {

/// \brief Context handed to the service's build callable.
///
/// Populated by bpt-app before the service is constructed. Kept
/// intentionally minimal — services that need additional pre-built
/// resources (credentials, sockets) can construct them inside their own
/// code using ctx.aeron.
struct AppContext {
    std::shared_ptr<::aeron::Aeron> aeron;  ///< nullptr if RunOptions::connect_aeron == false
    bpt::common::util::Topology topology;   ///< empty when base.topology_path is unset
};

/// \brief Service contract.
///
/// run() is expected to block on its own poll loop, checking
/// bpt::common::signal::is_running() for shutdown. stop() is called
/// after run() returns, for post-loop cleanup (draining queues,
/// flushing state) that shouldn't live in the destructor.
class IService {
public:
    virtual ~IService() = default;
    virtual void run() = 0;
    virtual void stop() {}
};

/// \brief Optional knobs for run().
///
/// Default-constructed RunOptions matches the historical behavior — every
/// existing caller keeps connecting to Aeron. Opt out per-service when
/// there's no Aeron consumer (e.g. bpt-tape captures WS frames to
/// disk only; nothing publishes).
struct RunOptions {
    /// Connect to the Aeron MediaDriver and populate AppContext::aeron.
    /// When false, AppContext::aeron is nullptr and the service must not
    /// dereference it.
    bool connect_aeron{true};
};

/// \brief Run the shared lifecycle.
///
/// Template parameters are deduced: \c Settings is the service's settings
/// struct (must have a `.base` member of type BaseSettings); \c BuildFn
/// is a callable `(Settings&, AppContext&) -> unique_ptr<IService>`.
///
/// \return 0 on clean shutdown, non-zero if a startup step fails.
///         Exceptions from \c build_fn propagate so the service can
///         decide whether to translate them; bpt-app doesn't swallow.
template <typename Settings, typename BuildFn>
int run(const std::string& service_name, Settings settings, BuildFn build_fn, RunOptions opts = {}) {
    auto& base = settings.base;

    bpt::common::signal::install();

    // Push service_name into the kernel's per-thread comm field so that
    // top/htop/ps -o comm/perf samples show role rather than binary name.
    // Linux TASK_COMM_LEN is 16 bytes incl. null → 15 usable chars; the
    // kernel silently truncates overflow. Role-qualified names (e.g.
    // "bpt-md-gw-okx") are constructed in each service's main() before
    // passing service_name here.
    ::prctl(PR_SET_NAME, service_name.substr(0, 15).c_str(), 0, 0, 0);

    bpt::common::logging::init(service_name, base.logging);

    // Log the resolved identity so log files are self-documenting about
    // what names the kernel actually sees after 15-char truncation — lets
    // an operator match a log archive back to `ps -o comm` / `top -H`
    // without reconstructing the truncation rules in their head, and
    // surfaces accidental collisions between venues/shards immediately.
    bpt::common::log::info("identity service={} comm={} backend_thread={}",
                           service_name,
                           service_name.substr(0, 15),
                           bpt::common::logging::backend_thread_name_for(service_name));

    bpt::common::log::info("Starting (env={})", to_string(base.environment));
    // Loud banner for prod so operators can't miss it when skimming logs
    // or confusing a prod session with qa. Uppercase + warning level for
    // attention. Only fires when env == PROD; qa/dev don't need it.
    if (base.is_prod()) {
        bpt::common::log::warn("================================================");
        bpt::common::log::warn(" RUNNING IN PROD  —  live capital is at risk ");
        bpt::common::log::warn("================================================");
    }

    if (base.calibrate_tsc)
        bpt::common::util::TscClock::calibrate();

    // Default Aeron error handler: log the exception + a capped backtrace.
    //
    // Most Aeron errors are recoverable — transient I/O issues, brief
    // contention, slow consumers. The conductor thread survives these
    // and the client continues. For those, log-and-continue is correct.
    //
    // A specific class of errors, however, indicates the client's
    // conductor state has become unrecoverable and the Aeron session
    // is effectively dead — further publications will silently no-op
    // until the process exits. Observed in production (laptop-sleep
    // event 2026-04-21 on WSL2) where a 5.5h stack outage went unnoticed
    // until MDGatewayQuiet fired: the process logged these errors in a
    // loop but couldn't publish a byte.
    //
    // On detection of one of those fatal patterns, log a clear marker
    // line and exit(1). systemd restarts the service, it rejoins the
    // MediaDriver cleanly, alerting surfaces the brief ServiceDown and
    // auto-resolves. Far better than silent-no-op for hours.
    auto aeron_error_handler = [svc = service_name](const std::exception& e) {
        const std::string_view msg = e.what();
        bpt::common::log::error("[Aeron] {}", msg);

        // Aeron error patterns we've observed as genuinely unrecoverable
        // (client state corrupted, no amount of retry from the conductor
        // thread will reattach). Add more as they're seen in the wild.
        static constexpr std::string_view kFatalPatterns[] = {
            "timeout between service calls",          // client conductor missed 20s service window
            "client heartbeat timestamp not active",  // MediaDriver declared the client dead
        };
        for (const auto& pat : kFatalPatterns) {
            if (msg.find(pat) != std::string_view::npos) {
                bpt::common::log::error("[Aeron] fatal — unrecoverable client state; exiting for systemd restart");
                // _Exit skips destructors (Aeron's shutdown path may itself
                // hit the conductor we just gave up on). systemd's
                // Restart=on-failure in the unit file brings us back.
                std::_Exit(1);
            }
        }

        // Non-fatal: log the backtrace for post-hoc analysis and carry on.
        void* frames[32];
        int n = ::backtrace(frames, 32);
        char** syms = ::backtrace_symbols(frames, n);
        for (int i = 0; i < n; ++i)
            bpt::common::log::error("  {}", syms ? syms[i] : "???");
        std::free(syms);
    };
    std::shared_ptr<::aeron::Aeron> aeron;
    if (opts.connect_aeron) {
        aeron = bpt::common::aeron::connect(base.media_driver_dir, aeron_error_handler);
        bpt::common::log::info("Connected to Aeron MediaDriver");
    } else {
        bpt::common::log::info("Aeron MediaDriver connection skipped (connect_aeron=false)");
    }

    auto topology = bpt::common::util::Topology::load(base.topology_path);
    if (topology.empty())
        bpt::common::log::info("CPU topology: empty (no pinning)");
    else
        bpt::common::log::info("CPU topology: loaded {} assignments from '{}'",
                               topology.assignment_count(),
                               base.topology_path);

    AppContext ctx{std::move(aeron), std::move(topology)};

    std::unique_ptr<IService> service = build_fn(settings, ctx);
    if (!service) {
        bpt::common::log::error("build callable returned null — aborting");
        return 1;
    }

    bpt::common::log::info("Ready — entering main loop");
    service->run();

    bpt::common::log::info("Signal received, shutting down");
    service->stop();

    return 0;
}

}  // namespace bpt::app
