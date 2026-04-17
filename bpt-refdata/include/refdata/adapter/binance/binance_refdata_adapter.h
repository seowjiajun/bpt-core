#pragma once

#include "refdata/adapter/binance/binance_parser.h"
#include "refdata/adapter/common/i_exchange_refdata_adapter.h"
#include "refdata/adapter/credentials.h"
#include "refdata/config/settings.h"
#include "refdata/http/rest_client.h"
#include "refdata/mapping/instrument_mapping_loader.h"
#include "refdata/registry/instrument_registry.h"

#include <atomic>
#include <memory>

namespace bpt::refdata::adapter {

// Binance REST reference data adapter.
//
// Snapshot (blocking, called on startup):
//   GET /api/v3/exchangeInfo      — spot instruments
//   GET /fapi/v1/exchangeInfo     — futures/perp instruments
//   GET /sapi/v1/asset/tradeFee   — fee schedules (requires API key)
//
// Funding rates have moved to Huginn (Binance !markPrice@arr@1s WS stream).
//
// Hourly poll:
//   Re-fetches /api/v3/exchangeInfo + /fapi/v1/exchangeInfo to detect listing changes.
class BinanceRefDataAdapter : public IExchangeRefDataAdapter {
public:
    BinanceRefDataAdapter(const config::AdapterConfig& cfg,
                          const ExchangeCredentials& creds,
                          std::shared_ptr<registry::InstrumentRegistry> registry,
                          std::shared_ptr<mapping::InstrumentMappingLoader> mapping);
    ~BinanceRefDataAdapter() override = default;

    void fetchSnapshot() override;
    void subscribeDeltas() override {}  // no-op: funding rates in Huginn
    void fetchInstrumentListing() override;
    void stop() override {}  // no-op: no WS thread

    bool isReady() const override { return ready_.load(std::memory_order_acquire); }
    const char* exchange_name() const override { return "BINANCE"; }
    bpt::messages::ExchangeId::Value exchange_id() const override { return bpt::messages::ExchangeId::BINANCE; }

private:
    config::AdapterConfig cfg_;
    std::shared_ptr<registry::InstrumentRegistry> registry_;
    std::atomic<bool> ready_{false};

    std::string api_key_;

    http::RestClient spot_client_;
    http::RestClient fapi_client_;
    BinanceParser parser_;
};

}  // namespace bpt::refdata::adapter
