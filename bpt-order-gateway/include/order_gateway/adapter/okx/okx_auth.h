#pragma once

// Pure, stateless OKX authentication helpers. No I/O, no config — every
// function takes its inputs explicitly so it is trivially unit-testable
// without spinning up an adapter.
//
// OKX auth is symmetric HMAC-SHA256 over a prehash string:
//   prehash = timestamp_s + HTTP_METHOD + request_path (+ body for POSTs)
//   sign    = base64(HMAC-SHA256(secret_key, prehash))
//
// The same recipe produces both the WS `login` message signature and
// the REST `OK-ACCESS-SIGN` header — the only difference is the path:
// WS login uses the magic literal "/users/self/verify".

#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>
#include <cstddef>
#include <string>
#include <string_view>

namespace bpt::order_gateway::adapter::okx {

// Base64 (single-line) encode arbitrary bytes. Used for HMAC output.
[[nodiscard]] std::string base64_encode(const unsigned char* data, std::size_t len);

// HMAC-SHA256(key, data) -> base64. Wraps the two-step for callers that
// don't care about the raw digest bytes.
[[nodiscard]] std::string hmac_sha256_b64(std::string_view key, std::string_view data);

// Build the OKX WS `{"op":"login","args":[...]}` envelope as a serialised
// JSON string. Uses std::chrono::system_clock for the timestamp so the
// sig is fresh each call.
[[nodiscard]] std::string build_login_msg(std::string_view api_key,
                                           std::string_view secret_key,
                                           std::string_view passphrase);

// Attach OK-ACCESS-{KEY,SIGN,TIMESTAMP,PASSPHRASE} headers + user-agent
// + host to a beast::http::request that's already been constructed with
// the target path and verb::get. Signs the prehash with a fresh
// timestamp. Sets `x-simulated-trading: 1` iff `testnet` is true —
// required for demo-trading REST calls.
void sign_get_request(boost::beast::http::request<boost::beast::http::string_body>& req,
                      std::string_view host,
                      std::string_view path,
                      std::string_view api_key,
                      std::string_view secret_key,
                      std::string_view passphrase,
                      bool testnet);

}  // namespace bpt::order_gateway::adapter::okx
