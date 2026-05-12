#pragma once

/// \file
/// \brief Hyperliquid REST reference-data adapter.

#include "refdata/adapter/common/i_exchange_refdata_adapter.h"
#include "refdata/adapter/credentials.h"
#include "refdata/adapter/hyperliquid/hyperliquid_refdata_decoder.h"
#include "refdata/config/settings.h"
#include "refdata/http/rest_client.h"
#include "refdata/mapping/instrument_mapping_loader.h"
#include "refdata/registry/instrument_registry.h"

#include <atomic>
#include <memory>

namespace bpt::refdata::adapter {

/// \brief Pulls HL instrument metadata + fees from the /info REST endpoint.
///
/// Snapshot (blocking, called on startup):
///   - `POST /info {"type":"meta"}` — all instrument metadata
///   - `POST /info {"type":"userFees"}` — fee schedule (requires wallet)
///
/// Hourly poll re-fetches /info meta to detect new/delisted instruments.
/// Funding rates live on the MdGateway side (HL activeAssetCtx WS channel)
/// — this adapter does not stream them.
class HyperliquidRefDataAdapter : public IExchangeRefDataAdapter {
public:
    HyperliquidRefDataAdapter(const config::AdapterConfig& cfg,
                              const ExchangeCredentials& creds,
                              std::shared_ptr<registry::InstrumentRegistry> registry,
                              std::shared_ptr<mapping::InstrumentMappingLoader> mapping,
                              std::shared_ptr<http::RestClient> client);
    ~HyperliquidRefDataAdapter() override = default;

    void fetchSnapshot() override;
    void subscribeDeltas() override {}  // no-op: funding rates in MdGateway
    void fetchInstrumentListing() override;
    void stop() override {}  // no-op: no WS thread

    bool isReady() const override { return ready_.load(std::memory_order_acquire); }
    const char* exchange_name() const override { return "HYPERLIQUID"; }
    bpt::messages::ExchangeId::Value exchange_id() const override { return bpt::messages::ExchangeId::HYPERLIQUID; }

private:
    config::AdapterConfig cfg_;
    std::shared_ptr<registry::InstrumentRegistry> registry_;
    std::atomic<bool> ready_{false};

    std::string wallet_address_;

    std::shared_ptr<http::RestClient> client_;
    HyperliquidRefdataDecoder decoder_;
};

}  // namespace bpt::refdata::adapter
