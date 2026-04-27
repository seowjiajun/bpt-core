#pragma once

#include "refdata/adapter/common/i_exchange_refdata_adapter.h"
#include "refdata/adapter/credentials.h"
#include "refdata/adapter/okx/okx_parser.h"
#include "refdata/config/settings.h"
#include "refdata/http/rest_client.h"
#include "refdata/mapping/instrument_mapping_loader.h"
#include "refdata/registry/instrument_registry.h"

#include <atomic>
#include <memory>

namespace bpt::refdata::adapter {

// OKX REST reference data adapter.
//
// Snapshot (blocking, called on startup):
//   GET /api/v5/public/instruments?instType=SPOT  — spot instruments
//   GET /api/v5/public/instruments?instType=SWAP  — perpetual swap instruments
//   GET /api/v5/account/trade-fee                 — fee schedules (requires API key)
//
// Funding rates have moved to MdGateway (OKX funding-rate WS channel).
//
// Hourly poll:
//   Re-fetches instruments endpoints to detect listing changes.
class OKXRefDataAdapter : public IExchangeRefDataAdapter {
public:
    OKXRefDataAdapter(const config::AdapterConfig& cfg,
                      const ExchangeCredentials& creds,
                      std::shared_ptr<registry::InstrumentRegistry> registry,
                      std::shared_ptr<mapping::InstrumentMappingLoader> mapping,
                      std::shared_ptr<http::RestClient> client);
    ~OKXRefDataAdapter() override = default;

    void fetchSnapshot() override;
    void subscribeDeltas() override {}  // no-op: funding rates in MdGateway
    void fetchInstrumentListing() override;
    void stop() override {}  // no-op: no WS thread

    bool isReady() const override { return ready_.load(std::memory_order_acquire); }
    const char* exchange_name() const override { return "OKX"; }
    bpt::messages::ExchangeId::Value exchange_id() const override { return bpt::messages::ExchangeId::OKX; }

private:
    config::AdapterConfig cfg_;
    std::shared_ptr<registry::InstrumentRegistry> registry_;
    std::atomic<bool> ready_{false};

    std::string api_key_;
    std::string secret_key_;
    std::string passphrase_;

    std::shared_ptr<http::RestClient> client_;
    OKXParser parser_;
};

}  // namespace bpt::refdata::adapter
