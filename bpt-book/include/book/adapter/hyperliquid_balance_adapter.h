#pragma once

#include "book/adapter/i_balance_adapter.h"

#include <string>

namespace bpt::book::adapter {

// Hyperliquid balance adapter. Hits two public /info endpoints:
//   - clearinghouseState   → perps sub-account (USDC-margined)
//   - spotClearinghouseState → spot sub-account (per-coin balances)
// Both are public — only the wallet address is required, no signing.
class HyperliquidBalanceAdapter final : public IBalanceAdapter {
public:
    struct Config {
        std::string rest_host;        // e.g. "api.hyperliquid-testnet.xyz"
        std::string rest_port{"443"};
        std::string wallet_address;   // EIP-55 hex address, public identifier
    };

    explicit HyperliquidBalanceAdapter(Config cfg);

    const char* venue_name() const override { return "HYPERLIQUID"; }
    std::vector<BalanceRow> fetch() override;

private:
    std::string post(const std::string& json_body) const;

    Config cfg_;
};

}  // namespace bpt::book::adapter
