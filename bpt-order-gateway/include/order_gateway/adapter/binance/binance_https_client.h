#pragma once

/// \file
/// \brief Synchronous HTTPS client for Binance's REST API.
///
/// Opens a fresh TLS connection per call — same rationale as OKX's
/// https_client: order placement is infrequent enough that pooling
/// isn't load-bearing and would only complicate failure handling.
///
/// All methods return the raw response body. Parsing is the caller's
/// job — the client stays independent of Binance's envelope shapes.

#include "order_gateway/adapter/common/beast_https_client.h"
#include "order_gateway/adapter/common/credentials.h"
#include "order_gateway/config/settings.h"

#include <string>

namespace bpt::order_gateway::adapter::binance {

class BinanceHttpsClient {
public:
    BinanceHttpsClient(const config::AdapterConfig& cfg, const ExchangeCredentials& creds);

    /// \brief Dispatch one request.
    ///
    /// \param with_api_key  Attaches the `X-MBX-APIKEY` header — required
    ///                      on all private endpoints, unused on public.
    /// \param body          Only read for POST/PUT; Binance puts most
    ///                      params in the query string of `path`, so the
    ///                      body is usually empty.
    std::string request(const std::string& method, const std::string& path, const std::string& body, bool with_api_key);

    [[nodiscard]] const std::string& api_key() const { return api_key_; }
    [[nodiscard]] const std::string& secret_key() const { return secret_key_; }

private:
    const config::AdapterConfig& cfg_;
    const std::string api_key_;
    const std::string secret_key_;
    common::BeastHttpsClient inner_;
};

}  // namespace bpt::order_gateway::adapter::binance
