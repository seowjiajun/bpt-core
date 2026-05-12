#pragma once

/// \file
/// \brief Pure, stateless OKX authentication helpers — no I/O, no config.
///
/// Every function takes its inputs explicitly so it is trivially
/// unit-testable without spinning up an adapter.
///
/// OKX auth is symmetric HMAC-SHA256 over a prehash string:
/// \code
///   prehash = timestamp_s + HTTP_METHOD + request_path (+ body for POSTs)
///   sign    = base64(HMAC-SHA256(secret_key, prehash))
/// \endcode
///
/// The same recipe produces both the WS `login` message signature and
/// the REST `OK-ACCESS-SIGN` header — the only difference is the path:
/// WS login uses the magic literal `/users/self/verify`.

#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>
#include <cstddef>
#include <string>
#include <string_view>

namespace bpt::order_gateway::adapter::okx {

/// \brief Build the OKX WS `{"op":"login","args":[...]}` envelope as a serialised JSON string.
///
/// Uses std::chrono::system_clock for the timestamp so the sig is fresh each call.
[[nodiscard]] std::string build_login_msg(std::string_view api_key,
                                          std::string_view secret_key,
                                          std::string_view passphrase);

/// \brief Attach OK-ACCESS-{KEY,SIGN,TIMESTAMP,PASSPHRASE} headers + user-agent + host
///        to a beast GET request already constructed with the target path.
///
/// Signs the prehash with a fresh timestamp. Sets `x-simulated-trading: 1`
/// iff `testnet` is true — required for demo-trading REST calls.
void sign_get_request(boost::beast::http::request<boost::beast::http::string_body>& req,
                      std::string_view host,
                      std::string_view path,
                      std::string_view api_key,
                      std::string_view secret_key,
                      std::string_view passphrase,
                      bool testnet);

}  // namespace bpt::order_gateway::adapter::okx
