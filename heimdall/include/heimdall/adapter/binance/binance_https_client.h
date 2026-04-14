#pragma once

// Synchronous HTTPS client for Binance's REST API. Opens a fresh TLS
// connection per call — same rationale as OKX's https_client: order
// placement is infrequent enough that pooling isn't load-bearing and
// would only complicate failure handling.
//
// All methods return the raw response body. Parsing is the caller's
// job — the client stays independent of Binance's envelope shapes.

#include "heimdall/adapter/common/beast_https_client.h"
#include "heimdall/adapter/common/credentials.h"
#include "heimdall/config/settings.h"

#include <string>

namespace heimdall::adapter::binance {

class BinanceHttpsClient {
public:
    BinanceHttpsClient(const config::AdapterConfig& cfg, const ExchangeCredentials& creds);

    // Dispatch one request. `with_api_key` attaches the X-MBX-APIKEY
    // header — required on all private endpoints, unused on public ones.
    // `body` is only read for POST/PUT; Binance puts most params in the
    // query string of `path` so the body is usually empty.
    std::string request(const std::string& method,
                         const std::string& path,
                         const std::string& body,
                         bool with_api_key);

    [[nodiscard]] const std::string& api_key() const { return api_key_; }
    [[nodiscard]] const std::string& secret_key() const { return secret_key_; }

private:
    const config::AdapterConfig& cfg_;
    const std::string api_key_;
    const std::string secret_key_;
    common::BeastHttpsClient inner_;
};

}  // namespace heimdall::adapter::binance
