#pragma once

/// \file
/// \brief ASCII string-case helpers shared across services.
///
/// Three near-identical `lowercase_venue` copies existed in
/// bpt-md-gateway, bpt-order-gateway, and bpt-tape before this header;
/// callers now share these definitions. Operates on ASCII via
/// `std::tolower`/`std::toupper`; bytes outside [0,127] are passed
/// through unchanged on most platforms but the result is not
/// locale-aware — matches the venue-naming use case (BTC, OKX, etc.).

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace bpt::common::util {

/// \brief Format a uint32_t as a fixed-width 8-char lowercase hex string.
///
/// Used by order-gateway venue adapters to derive a per-process session
/// prefix from `epoch_s` for client-order-id namespacing — ensures cloids
/// are unique across process restarts without any persistent state.
[[nodiscard]] inline std::string hex8(uint32_t v) noexcept {
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08x", v);
    return std::string(buf, 8);
}

[[nodiscard]] inline std::string to_lower(std::string_view s) {
    std::string out{s};
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

[[nodiscard]] inline std::string to_upper(std::string_view s) {
    std::string out{s};
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::toupper(c); });
    return out;
}

}  // namespace bpt::common::util
