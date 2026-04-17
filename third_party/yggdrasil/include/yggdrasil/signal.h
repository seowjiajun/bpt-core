#pragma once

// yggdrasil/signal.h — Unified signal handling for all Strategy services.
//
// Usage:
//   ygg::signal::install();          // call once in main
//   while (ygg::signal::is_running()) { ... }

#include <csignal>
#include <yggdrasil/logging.h>

namespace ygg::signal {

namespace detail {
// volatile sig_atomic_t is the only type safe to read/write from a signal
// handler.  inline ensures a single definition across all translation units.
inline volatile std::sig_atomic_t g_running = 1;
}  // namespace detail

inline void handler(int signum) {
    detail::g_running = 0;
    // Async logging from a signal handler is not strictly signal-safe,
    // but is acceptable here since we are already shutting down.
    ygg::log::info("Signal {} received, shutting down...", signum);
}

// Register SIGINT and SIGTERM handlers.  Call once at the top of main.
inline void install() {
    std::signal(SIGINT, handler);
    std::signal(SIGTERM, handler);
}

// Returns false once a SIGINT, SIGTERM, or stop() has been called.
inline bool is_running() noexcept {
    return detail::g_running != 0;
}

// Programmatic shutdown — equivalent to receiving a signal.
// Use when application logic determines it must halt (e.g. Strategy halting on
// missing RefDataReady exchanges).
inline void stop() noexcept {
    detail::g_running = 0;
}

}  // namespace ygg::signal
