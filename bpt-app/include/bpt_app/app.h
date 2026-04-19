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
#include <bpt_common/util/tsc_clock.h>

#include <execinfo.h>
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
    bpt::common::logging::init(service_name, base.logging);

    bpt::common::log::info("[{}] Starting (env={})",
                           service_name,
                           base.environment.empty() ? "(not set)" : base.environment);

    if (base.calibrate_tsc)
        bpt::common::util::TscClock::calibrate();

    // Default Aeron error handler: log the exception + a capped backtrace
    // and keep running (Aeron's conductor thread is designed to survive).
    // Services that want custom behaviour can bypass bpt::app::run() and
    // build their own Aeron client, but no current service does.
    auto aeron_error_handler = [svc = service_name](const std::exception& e) {
        bpt::common::log::error("[{}][Aeron] {}", svc, e.what());
        void* frames[32];
        int n = ::backtrace(frames, 32);
        char** syms = ::backtrace_symbols(frames, n);
        for (int i = 0; i < n; ++i)
            bpt::common::log::error("  {}", syms ? syms[i] : "???");
        std::free(syms);
    };
    auto aeron = bpt::common::aeron::connect(base.media_driver_dir, aeron_error_handler);
    bpt::common::log::info("[{}] Connected to Aeron MediaDriver", service_name);

    AppContext ctx{std::move(aeron)};

    std::unique_ptr<IService> service = build_fn(settings, ctx);
    if (!service) {
        bpt::common::log::error("[{}] build callable returned null — aborting", service_name);
        return 1;
    }

    bpt::common::log::info("[{}] Ready — entering main loop", service_name);
    service->run();

    bpt::common::log::info("[{}] Signal received, shutting down", service_name);
    service->stop();

    return 0;
}

}  // namespace bpt::app
