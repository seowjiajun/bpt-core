#include "bpt_app/base_settings.h"

#include <stdexcept>
#include <bpt_common/logging_toml.h>

namespace bpt::app {

void load_base_settings(const toml::table& root, BaseSettings& base) {
    // environment is required in TOML. Missing or unknown values throw
    // at boot rather than silently defaulting to DEV — a "prd" typo
    // would otherwise skip every prod-specific guard rail we rely on.
    auto env_str = root["environment"].value<std::string>();
    if (!env_str)
        throw std::runtime_error(
            "Missing required top-level key: environment = \"dev\" | \"qa\" | \"prod\"");
    base.environment = env_from_string(*env_str);

    if (const auto* a = root["aeron"].as_table()) {
        if (auto v = (*a)["media_driver_dir"].value<std::string>())
            base.media_driver_dir = *v;
    }

    if (const auto* l = root["logging"].as_table())
        base.logging = bpt::common::logging::from_toml(*l);

    if (const auto* m = root["metrics"].as_table()) {
        if (auto v = (*m)["port"].value<int64_t>())
            base.metrics_port = static_cast<uint16_t>(*v);
    }

    if (const auto* t = root["topology"].as_table()) {
        if (auto v = (*t)["path"].value<std::string>())
            base.topology_path = *v;
    }
}

}  // namespace bpt::app
