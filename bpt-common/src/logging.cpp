#include "bpt_common/logging.h"

#include <filesystem>
#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/backend/BackendOptions.h>
#include <quill/core/PatternFormatterOptions.h>
#include <quill/sinks/ConsoleSink.h>
#include <quill/sinks/RotatingFileSink.h>
#include <quill/sinks/RotatingSink.h>

namespace bpt::common::logging {

namespace {

quill::Logger* g_logger = nullptr;

// Captured at init() time and reused for every get_logger() call so
// sub-module loggers share the same sinks + pattern as the default.
std::vector<std::shared_ptr<quill::Sink>> g_sinks;
quill::PatternFormatterOptions g_fmt_opts;
quill::LogLevel g_default_level = quill::LogLevel::Info;

// Default pattern — includes the logger name in brackets so modules that
// create their own named logger are auto-prefixed, matching the manual
// [Service] prefix operators used to write in every log call. Services
// should let the pattern do the prefixing and drop the redundant manual
// bracket from message bodies.
constexpr const char* kDefaultPattern =
    "%(time) [%(log_level_short_code)] [%(logger)] %(message)";

// Linux TASK_COMM_LEN is 16 bytes incl. null terminator → 15 usable chars.
// Kept as a constant here rather than including <linux/sched.h> to keep
// bpt-common portable at build time.
constexpr std::size_t kMaxThreadNameChars = 15;

quill::LogLevel level_from_string(const std::string& s) {
    if (s == "trace")
        return quill::LogLevel::TraceL1;
    if (s == "debug")
        return quill::LogLevel::Debug;
    if (s == "info")
        return quill::LogLevel::Info;
    if (s == "warn" || s == "warning")
        return quill::LogLevel::Warning;
    if (s == "error")
        return quill::LogLevel::Error;
    if (s == "critical")
        return quill::LogLevel::Critical;
    if (s == "off")
        return quill::LogLevel::None;
    return quill::LogLevel::Info;
}

}  // namespace

quill::Logger* get_default_logger() {
    return g_logger;
}

std::string backend_thread_name_for(const std::string& service_name) {
    std::string name = service_name;
    if (name.rfind("bpt-", 0) == 0)
        name.erase(0, 4);
    for (std::string::size_type pos = 0; (pos = name.find("-gateway", pos)) != std::string::npos; pos += 3)
        name.replace(pos, 8, "-gw");
    name += "-log";
    if (name.size() > kMaxThreadNameChars)
        name.resize(kMaxThreadNameChars);
    return name;
}

void init(const std::string& service_name, const LogConfig& cfg) {
    quill::BackendOptions backend_opts;
    backend_opts.thread_name = backend_thread_name_for(service_name);
    if (cfg.flush_interval_ms > 0)
        backend_opts.sleep_duration = std::chrono::milliseconds(cfg.flush_interval_ms);
    quill::Backend::start(backend_opts);

    std::vector<std::shared_ptr<quill::Sink>> sinks;

    if (cfg.console) {
        auto console_sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("console");
        sinks.push_back(std::move(console_sink));
    }

    if (cfg.file) {
        std::filesystem::create_directories(cfg.log_dir);
        quill::RotatingFileSinkConfig rf_cfg;
        rf_cfg.set_rotation_max_file_size(static_cast<size_t>(cfg.max_file_size_mb) * 1024u * 1024u);
        rf_cfg.set_max_backup_files(cfg.max_files);
        auto file_sink =
            quill::Frontend::create_or_get_sink<quill::RotatingFileSink>(cfg.log_dir + "/" + service_name + ".log",
                                                                         rf_cfg);
        sinks.push_back(std::move(file_sink));
    }

    quill::PatternFormatterOptions fmt_opts;
    fmt_opts.format_pattern = cfg.pattern.empty() ? kDefaultPattern : cfg.pattern;

    // Cache for get_logger() so sub-module loggers share sinks + pattern.
    g_sinks = sinks;
    g_fmt_opts = fmt_opts;
    g_default_level = level_from_string(cfg.level);

    quill::Logger* logger = quill::Frontend::create_or_get_logger(service_name, std::move(sinks), fmt_opts);
    logger->set_log_level(g_default_level);

    g_logger = logger;
}

quill::Logger* get_logger(const std::string& name) {
    if (g_logger == nullptr)
        return nullptr;  // init() not yet called
    auto sinks = g_sinks;  // create_or_get_logger takes ownership; keep cache intact
    quill::Logger* logger = quill::Frontend::create_or_get_logger(name, std::move(sinks), g_fmt_opts);
    logger->set_log_level(g_default_level);
    return logger;
}

}  // namespace bpt::common::logging
