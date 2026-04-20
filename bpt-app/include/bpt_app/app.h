#pragma once

// bpt::app::run — shared application lifecycle. Every bpt-core service's
// main() should look like:
//
//   int main(int argc, char** argv) {
//       CLI::App cli{"my-service"};
//       std::string config_path = "config/my-service.toml";
//       cli.add_option("-c,--config", config_path)->check(CLI::ExistingFile);
//       CLI11_PARSE(cli, argc, argv);
//
//       auto settings = config::load(config_path);   // service-specific loader
//       return bpt::app::run("my-service", std::move(settings),
//           [](auto& cfg, auto& ctx) -> std::unique_ptr<bpt::app::IService> {
//               return std::make_unique<MyService>(std::move(cfg), ctx.aeron);
//           });
//   }
//
// What bpt::app::run owns:
//   - signal handler install (SIGINT / SIGTERM via bpt::common::signal)
//   - invariant-TSC calibration (if settings.base.calibrate_tsc)
//   - async Quill logging init (based on settings.base.logging)
//   - Aeron MediaDriver connection
//   - Main-thread service loop — service->run() blocks until signal fires
//   - Graceful shutdown — service->stop() on the way out
//
// What services still own:
//   - CLI argument definition (service-specific flags go here)
//   - Settings struct + TOML loader (which internally calls load_base_settings)
//   - The IService implementation (adapters, publishers, strategies, etc.)
//
// Settings requirements: the Settings type must expose a
// `bpt::app::BaseSettings base;` member. The template reads `settings.base`
// for lifecycle knobs before handing the full settings to the build callable.

#include "bpt_app/base_settings.h"

#include <Aeron.h>
#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>
#include <bpt_common/signal.h>
#include <bpt_common/util/topology.h>
#include <bpt_common/util/tsc_clock.h>

#include <execinfo.h>
#include <sys/prctl.h>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

namespace bpt::app {

// Context handed to the service's build callable. Populated by bpt-app
// before the service is constructed. Kept intentionally minimal — services
// that need additional pre-built resources (credentials, additional
// sockets) can construct them inside their own code using ctx.aeron.
struct AppContext {
    std::shared_ptr<::aeron::Aeron> aeron;
    bpt::common::util::Topology     topology;  // empty when base.topology_path is unset
};

// Service contract. run() is expected to block on its own poll loop,
// checking bpt::common::signal::is_running() for shutdown. stop() is
// called after run() returns, for any post-loop cleanup (draining queues,
// flushing state) that shouldn't be in the destructor.
class IService {
public:
    virtual ~IService() = default;
    virtual void run() = 0;
    virtual void stop() {}
};

// Run the shared lifecycle. Template parameters are deduced:
//   Settings — the service's settings struct, must have `.base` member
//   BuildFn  — callable taking (Settings&, AppContext&) -> unique_ptr<IService>
//
// Returns 0 on clean shutdown, non-zero if a startup step fails (exceptions
// from build_fn propagate so the service can decide; bpt-app does not swallow).
template <typename Settings, typename BuildFn>
int run(const std::string& service_name, Settings settings, BuildFn build_fn) {
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

    // Default Aeron error handler: log the exception + a capped backtrace
    // and keep running (Aeron's conductor thread is designed to survive).
    // Services that want custom behaviour can bypass bpt::app::run() and
    // build their own Aeron client, but no current service does.
    auto aeron_error_handler = [svc = service_name](const std::exception& e) {
        bpt::common::log::error("[Aeron] {}", e.what());
        void* frames[32];
        int n = ::backtrace(frames, 32);
        char** syms = ::backtrace_symbols(frames, n);
        for (int i = 0; i < n; ++i)
            bpt::common::log::error("  {}", syms ? syms[i] : "???");
        std::free(syms);
    };
    auto aeron = bpt::common::aeron::connect(base.media_driver_dir, aeron_error_handler);
    bpt::common::log::info("Connected to Aeron MediaDriver");

    auto topology = bpt::common::util::Topology::load(base.topology_path);
    if (topology.empty())
        bpt::common::log::info("CPU topology: empty (no pinning)");
    else
        bpt::common::log::info("CPU topology: loaded {} assignments from '{}'",
                               topology.assignment_count(), base.topology_path);

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
