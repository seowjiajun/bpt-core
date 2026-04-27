#pragma once

#include "refdata/adapter/common/i_exchange_refdata_adapter.h"
#include "refdata/adapter/credentials.h"
#include "refdata/adapter/deribit/deribit_parser.h"
#include "refdata/config/settings.h"
#include "refdata/http/rest_client.h"
#include "refdata/mapping/instrument_mapping_loader.h"
#include "refdata/registry/instrument_registry.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace bpt::refdata::adapter {

// Deribit REST (JSON-RPC 2.0) reference data adapter.
//
// Snapshot (blocking, called on startup):
//   POST /api/v2  method=public/get_instruments  — instruments per currency x kind
//   Fees: maker_commission / taker_commission returned in instrument data
//
// Funding rates have moved to MdGateway.
//
// Hourly poll:
//   Re-fetches instruments endpoints to detect listing changes.
class DeribitRefDataAdapter : public IExchangeRefDataAdapter {
public:
    DeribitRefDataAdapter(const config::AdapterConfig& cfg,
                          const ExchangeCredentials& creds,
                          std::shared_ptr<registry::InstrumentRegistry> registry,
                          std::shared_ptr<mapping::InstrumentMappingLoader> mapping,
                          std::shared_ptr<http::RestClient> rest_client);
    ~DeribitRefDataAdapter() override = default;

    void fetchSnapshot() override;
    void subscribeDeltas() override {}  // no-op: funding rates in MdGateway
    void fetchInstrumentListing() override;
    void stop() override {}  // no-op: no WS thread

    bool isReady() const override { return ready_.load(std::memory_order_acquire); }
    const char* exchange_name() const override { return "DERIBIT"; }
    bpt::messages::ExchangeId::Value exchange_id() const override { return bpt::messages::ExchangeId::DERIBIT; }

private:
    config::AdapterConfig cfg_;
    std::shared_ptr<registry::InstrumentRegistry> registry_;
    std::atomic<bool> ready_{false};

    std::string client_id_;
    std::string client_secret_;

    // Shared HTTPS client — reuses SSL context across calls and retries
    // on transient failures. Injected at construction; refdata_app picks
    // the host (defaulting to test.deribit.com when cfg_.rest_host empty).
    std::shared_ptr<bpt::refdata::http::RestClient> rest_client_;

    DeribitRefdataParser parser_;

    // Fetch + ingest one (currency, kind) pair. Used by both the
    // startup snapshot and the hourly listing refresh — the difference
    // between those paths is only whether we notify on delta changes.
    void ingest_instruments(const std::string& currency,
                            const std::string& kind,
                            uint64_t collected_ts,
                            bool notify_deltas);
};

}  // namespace bpt::refdata::adapter
