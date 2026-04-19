#pragma once

#include <map>
#include <string>

namespace bpt::order_gateway::adapter {

// Exchange credentials populated at startup from systemd-creds (or a local
// JSON file).  Passed directly to adapter constructors — never retained beyond
// adapter initialisation except inside the adapter itself.
struct ExchangeCredentials {
    std::string api_key;         // BINANCE, OKX
    std::string secret_key;      // BINANCE, OKX
    std::string passphrase;      // OKX only
    std::string private_key;     // HYPERLIQUID (64-char hex)
    std::string wallet_address;  // HYPERLIQUID (public address for account queries)
    std::string client_id;       // DERIBIT
    std::string client_secret;   // DERIBIT
};

// Map the flat key-value pairs returned by bpt::common::secrets::fetch into the fields
// relevant for the given exchange.  Unknown keys are silently ignored.
inline ExchangeCredentials credentials_from_secret(const std::string& exchange,
                                                   const std::map<std::string, std::string>& kv) {
    const auto get = [&](const char* key) -> std::string {
        const auto it = kv.find(key);
        return it != kv.end() ? it->second : std::string{};
    };

    ExchangeCredentials c;
    if (exchange == "BINANCE") {
        c.api_key = get("BINANCE_API_KEY");
        c.secret_key = get("BINANCE_SECRET_KEY");
    } else if (exchange == "OKX") {
        c.api_key = get("OKX_API_KEY");
        c.secret_key = get("OKX_SECRET_KEY");
        c.passphrase = get("OKX_PASSPHRASE");
    } else if (exchange == "HYPERLIQUID") {
        c.private_key = get("HYPERLIQUID_PRIVATE_KEY");
        c.wallet_address = get("HYPERLIQUID_WALLET_ADDRESS");
    } else if (exchange == "DERIBIT") {
        c.client_id = get("DERIBIT_CLIENT_ID");
        c.client_secret = get("DERIBIT_CLIENT_SECRET");
    }
    return c;
}

}  // namespace bpt::order_gateway::adapter
