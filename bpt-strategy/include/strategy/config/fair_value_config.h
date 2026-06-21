#pragma once

#include <features/fair_value.h>
#include <toml++/toml.h>

namespace bpt::strategy::config {

[[nodiscard]] inline bpt::features::FairValueEstimator::Config parse_fv_config(const toml::table& params) {
    bpt::features::FairValueEstimator::Config c;
    const auto fv = params["fair_value"];
    if (!fv)
        return c;
    const std::string mode = fv["mode"].value<std::string>().value_or("mid");
    if (mode == "mid")
        c.mode = bpt::features::FairValueEstimator::Mode::kMid;
    else if (mode == "micro")
        c.mode = bpt::features::FairValueEstimator::Mode::kMicro;
    else if (mode == "micro_capped")
        c.mode = bpt::features::FairValueEstimator::Mode::kMicroSizeCapped;
    else if (mode == "l2_weighted")
        c.mode = bpt::features::FairValueEstimator::Mode::kL2WeightedMicro;
    else if (mode == "ewma_micro")
        c.mode = bpt::features::FairValueEstimator::Mode::kEwmaMicro;
    c.size_cap_qty = fv["size_cap_qty"].value<double>().value_or(c.size_cap_qty);
    c.ladder_depth =
        static_cast<std::size_t>(fv["ladder_depth"].value<int64_t>().value_or(static_cast<int64_t>(c.ladder_depth)));
    c.ladder_decay = fv["ladder_decay"].value<double>().value_or(c.ladder_decay);
    c.ewma_alpha = fv["ewma_alpha"].value<double>().value_or(c.ewma_alpha);
    return c;
}

}  // namespace bpt::strategy::config
