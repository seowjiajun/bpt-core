#pragma once

// Synchronous HTTPS client for OKX's REST API. Opens a fresh TLS
// connection per call — OKX REST is infrequent (startup instrument
// fetch + periodic account snapshot), so the extra handshake cost is
// not load-bearing and connection pooling would only complicate
// failure handling.
//
// Two call styles:
//   - get_unsigned(path): public endpoints like /api/v5/public/instruments
//   - get_signed(path):   private endpoints, attaches OK-ACCESS-* headers
//
// Both return the raw response body. Parsing is the caller's job —
// this keeps the client from needing to know about OKX's envelope
// shapes or the wire types each endpoint produces.

#include "heimdall/adapter/common/beast_https_client.h"
#include "heimdall/adapter/common/credentials.h"
#include "heimdall/config/settings.h"

#include <string>

namespace heimdall::adapter::okx {

class OKXHttpsClient {
public:
    OKXHttpsClient(const config::AdapterConfig& cfg, const ExchangeCredentials& creds);

    // Unauthenticated GET. Testnet flag is honoured via the
    // `x-simulated-trading: 1` header so demo-trading public instrument
    // lookups route to the demo endpoints.
    [[nodiscard]] std::string get_unsigned(const std::string& path);

    // Authenticated GET. Fresh signature per call. Throws on transport
    // errors; HTTP-level errors (non-2xx, OKX code != "0") are surfaced
    // in the response body as-is for the caller to inspect.
    [[nodiscard]] std::string get_signed(const std::string& path);

private:
    const config::AdapterConfig& cfg_;
    const std::string api_key_;
    const std::string secret_key_;
    const std::string passphrase_;
    common::BeastHttpsClient inner_;
};

}  // namespace heimdall::adapter::okx
