#pragma once

// OKX refdata REST request signer — pure, stateless. The same HMAC-SHA256
// recipe used by order-gw's OKX auth (bpt-order-gateway/adapter/okx/okx_auth),
// but separate because refdata's RestClient + header type is a different
// code path that doesn't depend on Beast.
//
//   prehash = timestamp_iso8601 + HTTP_METHOD + request_path (+ body for POSTs)
//   sign    = base64(HMAC-SHA256(secret_key, prehash))

#include "refdata/http/rest_client.h"

#include <string>

namespace bpt::refdata::adapter {

// Build the OK-ACCESS-{KEY,SIGN,TIMESTAMP,PASSPHRASE} header block for a
// REST call. `method` is "GET" or "POST"; `target` is the request path
// including any query string. When `simulated` is true, adds the
// `x-simulated-trading: 1` header required by OKX demo-trading endpoints.
//
// No body-signing overload yet — refdata only issues GETs against OKX.
// Extend if/when a POST is needed.
http::RestClient::Headers okx_auth_headers(const std::string& api_key,
                                           const std::string& secret_key,
                                           const std::string& passphrase,
                                           const std::string& method,
                                           const std::string& target,
                                           bool simulated = false);

}  // namespace bpt::refdata::adapter
