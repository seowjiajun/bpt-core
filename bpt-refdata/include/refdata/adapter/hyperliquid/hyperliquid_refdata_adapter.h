#pragma once

#include "refdata/adapter/common/i_exchange_refdata_adapter.h"
#include "refdata/adapter/credentials.h"
#include "refdata/adapter/hyperliquid/hyperliquid_parser.h"
#include "refdata/config/settings.h"
#include "refdata/http/rest_client.h"
#include "refdata/mapping/instrument_mapping_loader.h"
#include "refdata/registry/instrument_registry.h"

#include <atomic>
#include <memory>

namespace bpt::refdata::adapter {

// Hyperliquid REST reference data adapter.
//
// Snapshot (blocking, called on startup):
//   POST /info {"type":"meta"}             — all instrument metadata
//   POST /info {"type":"userFees"}         — fee schedule (requires wallet address)
//
// Funding rates have moved to Huginn (Hyperliquid activeAssetCtx WS channel).
//
// Hourly poll:
//   Re-fetches /info meta to detect new/delisted instruments.
class HyperliquidRefDataAdapter : public IExchangeRefDataAdapter {
public:
    HyperliquidRefDataAdapter(const config::AdapterConfig& cfg,
                              const ExchangeCredentials& creds,
                              std::shared_ptr<registry::InstrumentRegistry> registry,
                              std::shared_ptr<mapping::InstrumentMappingLoader> mapping);
    ~HyperliquidRefDataAdapter() override = default;

    void fetchSnapshot() override;
    void subscribeDeltas() override {}  // no-op: funding rates in Huginn
    void fetchInstrumentListing() override;
    void stop() override {}  // no-op: no WS thread

    bool isReady() const override { return ready_.load(std::memory_order_acquire); }
    const char* exchange_name() const override { return "HYPERLIQUID"; }
    bpt::messages::ExchangeId::Value exchange_id() const override {
        return bpt::messages::ExchangeId::HYPERLIQUID;
    }

private:
    config::AdapterConfig cfg_;
    std::shared_ptr<registry::InstrumentRegistry> registry_;
    std::atomic<bool> ready_{false};

    std::string wallet_address_;

    http::RestClient client_;
    HyperliquidParser parser_;
};

}  // namespace bpt::refdata::adapter
