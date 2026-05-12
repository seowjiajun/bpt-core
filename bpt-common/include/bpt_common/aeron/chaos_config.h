#pragma once

/// \file
/// \brief TOML loader for the Aeron chaos registry.
///
/// Kept separate from `chaos_filter.h` so consumers that just need
/// the runtime side (Subscriber + ChaosRegistry::wrap) don't transitively
/// pull toml++ headers — same split pattern as `logging_toml.h`.
///
/// TOML shape:
///
/// \code{.toml}
/// [chaos.drop_prob_by_stream_id]
/// 1003 = 1.0   # refdata-delta dead — verify strategy keeps trading on cached snapshot
/// 5001 = 0.5   # 50% of analytics ToxicityScore drops
/// \endcode
///
/// Stream IDs MUST be quoted as TOML keys (TOML requires string keys),
/// e.g. `"1003" = 1.0`. The loader parses the string back to int32.
///
/// Prod gate: if `env == "prod"` and any drop entry is non-zero, the
/// loader throws. There is no flag to bypass this — chaos in prod is
/// always a misconfiguration, not a feature.

#include "bpt_common/aeron/chaos_filter.h"
#include "bpt_common/logging.h"

#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <toml++/toml.hpp>

namespace bpt::common::aeron {

/// \brief Load `[chaos]` from `root` into a ChaosConfig.
/// \param root parsed top-level TOML table for the service config
/// \param env  effective `BPT_ENV` (or `Settings::base.environment`); used for the prod gate
/// \return populated ChaosConfig (empty if `[chaos]` section is missing)
/// \throws std::invalid_argument on malformed values (out-of-range probability, non-int key)
/// \throws std::runtime_error if `env == "prod"` and any drop is configured
inline ChaosConfig load_chaos_config(const toml::table& root, std::string_view env) {
    ChaosConfig out;
    auto chaos = root.get_as<toml::table>("chaos");
    if (!chaos)
        return out;

    auto by_stream = chaos->get_as<toml::table>("drop_prob_by_stream_id");
    if (!by_stream)
        return out;

    for (const auto& [k, v] : *by_stream) {
        std::int32_t stream_id;
        try {
            stream_id = static_cast<std::int32_t>(std::stoi(std::string{k.str()}));
        } catch (const std::exception&) {
            throw std::invalid_argument(
                fmt::format("[chaos.drop_prob_by_stream_id] key must be an int (got '{}')", std::string{k.str()}));
        }
        const double p = v.value_or(-1.0);
        if (p < 0.0 || p > 1.0) {
            throw std::invalid_argument(
                fmt::format("[chaos.drop_prob_by_stream_id] stream {} drop_prob must be in [0,1], got {}",
                            stream_id,
                            p));
        }
        if (p > 0.0) {
            out.drop_prob_by_stream_id[stream_id] = p;
        }
    }

    if (env == "prod" && !out.empty()) {
        throw std::runtime_error("[chaos] config rejected in prod environment — chaos injection is dev/qa only");
    }

    return out;
}

/// \brief Parse `[chaos]` from `config_path`, log + install into the registry.
///
/// Convenience wrapper that turns the three-line "parse + warn + set_global"
/// dance every service's main() would otherwise repeat into a single call.
/// On any non-empty config, ensures logging is initialised so the WARN
/// lines actually surface (chaos load runs before bpt::app::run, which
/// is the usual logging-init site).
///
/// \param config_path   path to the same TOML the service is loading
/// \param env           "dev" / "qa" / "prod" (use `to_string(base.environment)`)
/// \param service_name  passed to logging::init if a non-empty config triggers it
/// \return number of streams configured for fault injection (0 = no [chaos] block)
/// \throws on parse error, malformed value, or prod-gate trip — caller's
///         existing try/catch around config load handles it.
inline std::size_t install_chaos_from_toml(const std::string& config_path,
                                           std::string_view env,
                                           std::string_view service_name) {
    auto root = toml::parse_file(config_path);
    auto cfg = load_chaos_config(root, env);
    if (cfg.empty()) {
        return 0;
    }
    bpt::common::logging::init(std::string{service_name});
    for (const auto& [stream_id, p] : cfg.drop_prob_by_stream_id) {
        bpt::common::log::warn("[chaos] stream {} drop_prob={:.2f} — fault injection active", stream_id, p);
    }
    const auto count = cfg.drop_prob_by_stream_id.size();
    ChaosRegistry::set_global(std::move(cfg));
    return count;
}

}  // namespace bpt::common::aeron
