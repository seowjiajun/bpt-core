#pragma once

// yggdrasil/logging.h — Shared logger config, initialisation, and log call API.
//
// All services call bpt::common::log::info/warn/error/debug/trace — never the backend
// directly.  Swapping backends (e.g. spdlog → Quill) is a one-file change in
// logging.cpp; no service code is touched.
//
// Usage:
//   #include <bpt_common/logging.h>
//
//   bpt::common::logging::init("fenrir");          // once at startup
//   bpt::common::log::info("tick {}", n);          // anywhere
//
// To load LogConfig from a TOML table, include <yggdrasil/logging_toml.h>.
//
// Backend: Quill (current).
//
// Hot-path note: Quill's zero-copy arg serialisation requires LOG_* macros with
// a compile-time string literal at the call site.  Our function-call API
// pre-formats the message with fmt (calling thread) then enqueues the result as
// a std::string through Quill's SPSC queue.  The calling-thread overhead is the
// same as spdlog async; the gain is Quill's faster SPSC queue and I/O backend.
// If the extra ~50-200ns per log call ever becomes a bottleneck, switch call
// sites to the LOG_INFO(bpt::common::logging::get_default_logger(), ...) macros.

#include <cstdint>
#include <fmt/format.h>
#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <string>

namespace bpt::common::logging {

struct LogConfig {
    std::string log_dir = "logs";
    std::string level = "info";        // trace/debug/info/warn/error/critical/off
    std::string flush_level = "warn";  // note: per-level flush is not supported by the Quill
                                       // backend; this field is reserved for future use
    bool console = true;
    bool file = true;
    uint32_t async_queue_size = 131072;  // initial SPSC queue capacity in bytes (Quill default: 128 KiB)
    uint32_t async_threads = 1;
    bool block_on_overflow = false;  // false = drop oldest when queue full
    uint32_t max_file_size_mb = 10;
    uint32_t max_files = 3;          // number of rotated files to retain
    std::string pattern;             // empty = Quill default pattern
    uint32_t flush_interval_ms = 0;  // 0 = no periodic flush; >0 = backend wakes every N ms
};

// Returns the Quill logger created by init().  Nullptr until init() is called.
quill::Logger* get_default_logger();

// Create or fetch a named sub-module logger. Shares sinks + pattern with
// the default logger — output is auto-prefixed with the given name via
// the %(logger) placeholder in the pattern, so modules don't have to
// hand-prefix every log line. Returns nullptr if called before init().
//
// Safe to call once per module and cache the pointer as a file-scope
// static — Quill's create_or_get_logger is idempotent by name.
quill::Logger* get_logger(const std::string& name);

void init(const std::string& service_name, const LogConfig& cfg = {});

}  // namespace bpt::common::logging

// ── Log call API ─────────────────────────────────────────────────────────────
//
// The format string is validated at compile time by fmt::format_string<Args...>.
// The message is pre-formatted on the calling thread and enqueued as a
// std::string via Quill's SPSC lock-free queue.  The backend thread handles
// all sink I/O.
//
namespace bpt::common::log {

template <typename... Args>
inline void trace(fmt::format_string<Args...> fmt, Args&&... args) {
    auto* l = bpt::common::logging::get_default_logger();
    if (l) {
        auto msg = fmt::format(fmt, std::forward<Args>(args)...);
        LOG_TRACE_L1(l, "{}", msg);
    }
}

template <typename... Args>
inline void debug(fmt::format_string<Args...> fmt, Args&&... args) {
    auto* l = bpt::common::logging::get_default_logger();
    if (l) {
        auto msg = fmt::format(fmt, std::forward<Args>(args)...);
        LOG_DEBUG(l, "{}", msg);
    }
}

template <typename... Args>
inline void info(fmt::format_string<Args...> fmt, Args&&... args) {
    auto* l = bpt::common::logging::get_default_logger();
    if (l) {
        auto msg = fmt::format(fmt, std::forward<Args>(args)...);
        LOG_INFO(l, "{}", msg);
    }
}

template <typename... Args>
inline void warn(fmt::format_string<Args...> fmt, Args&&... args) {
    auto* l = bpt::common::logging::get_default_logger();
    if (l) {
        auto msg = fmt::format(fmt, std::forward<Args>(args)...);
        LOG_WARNING(l, "{}", msg);
    }
}

template <typename... Args>
inline void error(fmt::format_string<Args...> fmt, Args&&... args) {
    auto* l = bpt::common::logging::get_default_logger();
    if (l) {
        auto msg = fmt::format(fmt, std::forward<Args>(args)...);
        LOG_ERROR(l, "{}", msg);
    }
}

template <typename... Args>
inline void critical(fmt::format_string<Args...> fmt, Args&&... args) {
    auto* l = bpt::common::logging::get_default_logger();
    if (l) {
        auto msg = fmt::format(fmt, std::forward<Args>(args)...);
        LOG_CRITICAL(l, "{}", msg);
    }
}

// ── Per-logger API ───────────────────────────────────────────────────────────
//
// For modules that want their own prefix (e.g. strategy sub-components
// [AS]/[OFI]/[Reconciler]) separate from the service-level default.
// Obtain a logger via bpt::common::logging::get_logger(name), cache it
// as a file-scope static, then call these helpers. Pattern auto-includes
// the logger name, so the message body should NOT hand-prefix anymore.

template <typename... Args>
inline void trace(quill::Logger* l, fmt::format_string<Args...> fmt, Args&&... args) {
    if (l) {
        auto msg = fmt::format(fmt, std::forward<Args>(args)...);
        LOG_TRACE_L1(l, "{}", msg);
    }
}

template <typename... Args>
inline void debug(quill::Logger* l, fmt::format_string<Args...> fmt, Args&&... args) {
    if (l) {
        auto msg = fmt::format(fmt, std::forward<Args>(args)...);
        LOG_DEBUG(l, "{}", msg);
    }
}

template <typename... Args>
inline void info(quill::Logger* l, fmt::format_string<Args...> fmt, Args&&... args) {
    if (l) {
        auto msg = fmt::format(fmt, std::forward<Args>(args)...);
        LOG_INFO(l, "{}", msg);
    }
}

template <typename... Args>
inline void warn(quill::Logger* l, fmt::format_string<Args...> fmt, Args&&... args) {
    if (l) {
        auto msg = fmt::format(fmt, std::forward<Args>(args)...);
        LOG_WARNING(l, "{}", msg);
    }
}

template <typename... Args>
inline void error(quill::Logger* l, fmt::format_string<Args...> fmt, Args&&... args) {
    if (l) {
        auto msg = fmt::format(fmt, std::forward<Args>(args)...);
        LOG_ERROR(l, "{}", msg);
    }
}

template <typename... Args>
inline void critical(quill::Logger* l, fmt::format_string<Args...> fmt, Args&&... args) {
    if (l) {
        auto msg = fmt::format(fmt, std::forward<Args>(args)...);
        LOG_CRITICAL(l, "{}", msg);
    }
}

}  // namespace bpt::common::log
