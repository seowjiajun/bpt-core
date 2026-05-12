#include "bpt_common/env.h"

#include <fmt/format.h>
#include <stdexcept>

namespace bpt::common {

std::string_view to_string(Env e) noexcept {
    switch (e) {
        case Env::DEV:
            return "dev";
        case Env::QA:
            return "qa";
        case Env::PROD:
            return "prod";
    }
    return "dev";  // unreachable — silence compiler warning
}

Env env_from_string(std::string_view s) {
    if (s == "dev")
        return Env::DEV;
    if (s == "qa")
        return Env::QA;
    if (s == "prod")
        return Env::PROD;
    throw std::runtime_error(fmt::format("Invalid environment \"{}\" — must be one of: dev, qa, prod", s));
}

}  // namespace bpt::common
