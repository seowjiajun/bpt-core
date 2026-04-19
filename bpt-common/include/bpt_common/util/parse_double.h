#pragma once

// yggdrasil/parse_double.h — fast_float wrapper for quoted-number JSON fields.
//
// Dependencies: fast_float, simdjson (provided by the consuming project).
//
// Usage:
//   double val = 0;
//   if (bpt::common::util::ff_double(field, val)) return;  // non-zero = error

#include <fast_float/fast_float.h>
#include <simdjson.h>
#include <system_error>

namespace bpt::common::util {

// Parse a quoted JSON number string using fast_float (Eisel-Lemire algorithm).
// Drop-in replacement for simdjson's get_double_in_string().get(out):
//
//   Before: (void) field.get_double_in_string().get(val);
//   After:  (void) bpt::common::util::ff_double(field, val);
//
// Returns simdjson::SUCCESS (0) on success, non-zero on error — matching
// simdjson's convention so existing if/void patterns work unchanged.
inline simdjson::error_code ff_double(simdjson::simdjson_result<simdjson::ondemand::value> v, double& out) noexcept {
    std::string_view sv;
    auto err = v.get_string().get(sv);
    if (err)
        return err;
    auto res = fast_float::from_chars(sv.data(), sv.data() + sv.size(), out);
    return (res.ec == std::errc{}) ? simdjson::SUCCESS : simdjson::NUMBER_ERROR;
}

}  // namespace bpt::common::util
