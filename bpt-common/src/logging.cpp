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

void init(const std::string& service_name, const LogConfig& cfg) {
    quill::BackendOptions backend_opts;
    backend_opts.thread_name = "quill-backend";
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
    if (!cfg.pattern.empty())
        fmt_opts.format_pattern = cfg.pattern;

    quill::Logger* logger = quill::Frontend::create_or_get_logger(service_name, std::move(sinks), fmt_opts);
    logger->set_log_level(level_from_string(cfg.level));

    g_logger = logger;
}

}  // namespace bpt::common::logging
