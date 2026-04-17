#pragma once

// Binance REST auth — query-string HMAC-SHA256 with hex output.
//
// Every private endpoint takes the request params in the URL query
// string, with `&timestamp=<ms>&signature=<hex>` appended. The signature
// is computed over the full "params&timestamp=<ms>" string. No
// header-based auth like OKX — the only extra header is `X-MBX-APIKEY`.

#include <string>
#include <string_view>

namespace bpt::order_gateway::adapter::binance {

// HMAC-SHA256(key, data) -> hex. Binance's signature format.
[[nodiscard]] std::string hmac_sha256_hex(std::string_view key, std::string_view data);

// Append `&timestamp=<now_ms>&signature=<hmac-hex>` to the given query
// param string. Callers construct the unsigned params via the action
// codec, then hand them here right before sending the request so the
// timestamp is as fresh as possible.
[[nodiscard]] std::string sign_query(std::string_view secret_key, const std::string& params);

}  // namespace bpt::order_gateway::adapter::binance
