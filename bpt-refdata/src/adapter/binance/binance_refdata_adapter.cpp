#include "refdata/adapter/binance/binance_refdata_adapter.h"

#include <messages/DeltaUpdateType.h>

#include <bpt_common/util/tsc_clock.h>

namespace bpt::refdata::adapter {

namespace {

uint64_t now_ns() {
    return bpt::common::util::TscClock::now_epoch_ns();
}

}  // namespace

BinanceRefDataAdapter::BinanceRefDataAdapter(const config::AdapterConfig& cfg,
                                             const ExchangeCredentials& creds,
                                             std::shared_ptr<registry::InstrumentRegistry> registry,
                                             std::shared_ptr<mapping::InstrumentMappingLoader> mapping)
    : cfg_(cfg),
      registry_(std::move(registry)),
      api_key_(creds.api_key),
      spot_client_(cfg.rest_host.empty() ? "api.binance.com" : cfg.rest_host, cfg.rest_port, cfg.use_tls),
      fapi_client_(cfg.rest_host.empty() ? "fapi.binance.com" : cfg.rest_host, cfg.rest_port, cfg.use_tls),
      parser_(mapping) {}

void BinanceRefDataAdapter::fetchSnapshot() {
    bpt::common::log::info("[BinanceRefData] Starting snapshot fetch...");
    const uint64_t ts = now_ns();

    // 1. Spot instruments
    try {
        auto body = spot_client_.get("/api/v3/exchangeInfo");
        for (auto& inst : parser_.parse_spot_exchange_info(body, ts))
            registry_->add(inst);
    } catch (const std::exception& e) {
        bpt::common::log::error("[BinanceRefData] Failed to fetch spot exchangeInfo: {}", e.what());
        throw;
    }

    // 2. Futures / perp instruments
    try {
        auto body = fapi_client_.get("/fapi/v1/exchangeInfo");
        for (auto& inst : parser_.parse_futures_exchange_info(body, ts))
            registry_->add(inst);
    } catch (const std::exception& e) {
        bpt::common::log::error("[BinanceRefData] Failed to fetch fapi exchangeInfo: {}", e.what());
        // Non-fatal — continue without perp instruments
    }

    // 3. Trade fee (requires API key)
    if (!api_key_.empty()) {
        try {
            auto body = spot_client_.get("/sapi/v1/asset/tradeFee", {{"X-MBX-APIKEY", api_key_}});
            for (auto& fs : parser_.parse_trade_fee(body, ts))
                if (on_fee_schedule)
                    on_fee_schedule(fs);
        } catch (const std::exception& e) {
            bpt::common::log::warn("[BinanceRefData] Failed to fetch trade fee (continuing): {}", e.what());
        }
    } else {
        bpt::common::log::warn("[BinanceRefData] No API key configured — skipping trade fee fetch");
    }

    ready_.store(true, std::memory_order_release);
    bpt::common::log::info("[BinanceRefData] Snapshot complete. Registry has {} instruments.", registry_->getAll().size());
}

void BinanceRefDataAdapter::fetchInstrumentListing() {
    bpt::common::log::info("[BinanceRefData] Hourly instrument listing refresh...");
    const uint64_t ts = now_ns();

    auto notify = [this, ts](refdata::Instrument& inst) {
        if (registry_->update_if_changed(inst) && on_instrument_delta)
            on_instrument_delta(inst, bpt::messages::DeltaUpdateType::MODIFY, ts);
    };

    try {
        auto body = spot_client_.get("/api/v3/exchangeInfo");
        for (auto& inst : parser_.parse_spot_exchange_info(body, ts))
            notify(inst);
    } catch (const std::exception& e) {
        bpt::common::log::error("[BinanceRefData] Hourly spot refresh failed: {}", e.what());
    }

    try {
        auto body = fapi_client_.get("/fapi/v1/exchangeInfo");
        for (auto& inst : parser_.parse_futures_exchange_info(body, ts))
            notify(inst);
    } catch (const std::exception& e) {
        bpt::common::log::error("[BinanceRefData] Hourly futures refresh failed: {}", e.what());
    }
}

}  // namespace bpt::refdata::adapter
