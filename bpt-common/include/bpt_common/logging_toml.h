#pragma once

// yggdrasil/logging_toml.h — TOML loader for LogConfig.
//
// Include this only in config-loading translation units (e.g. config/loader.cpp).
// It pulls in toml++ headers; keeping it separate avoids inflating compile times
// for files that only need to call bpt::common::logging::init().
//
// Expected TOML schema (all fields optional; defaults match LogConfig defaults):
//
//   [logging]
//   level             = "info"    # trace/debug/info/warn/error/critical/off
//   dir               = "logs"
//   flush_level       = "warn"
//   console           = true
//   file              = true
//   max_file_size_mb  = 10
//   max_files         = 3
//   async_queue_size  = 8192
//   block_on_overflow = false
//   flush_interval_ms = 0
//   # pattern         = ""        # custom spdlog format string (omit for default)

#include <bpt_common/logging.h>
#include <toml++/toml.hpp>

namespace bpt::common::logging {

inline LogConfig from_toml(const toml::table& t) {
    LogConfig cfg;
    if (auto v = t["level"].value<std::string>())
        cfg.level = *v;
    if (auto v = t["dir"].value<std::string>())
        cfg.log_dir = *v;
    if (auto v = t["flush_level"].value<std::string>())
        cfg.flush_level = *v;
    if (auto v = t["console"].value<bool>())
        cfg.console = *v;
    if (auto v = t["file"].value<bool>())
        cfg.file = *v;
    if (auto v = t["max_file_size_mb"].value<uint32_t>())
        cfg.max_file_size_mb = *v;
    if (auto v = t["max_files"].value<uint32_t>())
        cfg.max_files = *v;
    if (auto v = t["async_queue_size"].value<uint32_t>())
        cfg.async_queue_size = *v;
    if (auto v = t["block_on_overflow"].value<bool>())
        cfg.block_on_overflow = *v;
    if (auto v = t["flush_interval_ms"].value<uint32_t>())
        cfg.flush_interval_ms = *v;
    if (auto v = t["pattern"].value<std::string>())
        cfg.pattern = *v;
    return cfg;
}

}  // namespace bpt::common::logging
