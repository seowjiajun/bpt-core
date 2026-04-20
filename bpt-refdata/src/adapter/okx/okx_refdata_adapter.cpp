#include "refdata/adapter/okx/okx_refdata_adapter.h"

#include "refdata/adapter/okx/okx_refdata_auth.h"

#include <messages/DeltaUpdateType.h>

#include <bpt_common/logging.h>
#include <bpt_common/util/tsc_clock.h>

namespace bpt::refdata::adapter {

namespace {

uint64_t now_ns() {
    return bpt::common::util::TscClock::now_epoch_ns();
}

quill::Logger* kLog() {
    static quill::Logger* l = bpt::common::logging::get_logger("OKXRefData");
    return l;
}

}  // namespace

OKXRefDataAdapter::OKXRefDataAdapter(const config::AdapterConfig& cfg,
                                     const ExchangeCredentials& creds,
                                     std::shared_ptr<registry::InstrumentRegistry> registry,
                                     std::shared_ptr<mapping::InstrumentMappingLoader> mapping)
    : cfg_(cfg),
      registry_(std::move(registry)),
      api_key_(creds.api_key),
      secret_key_(creds.secret_key),
      passphrase_(creds.passphrase),
      client_(cfg.rest_host.empty() ? "www.okx.com" : cfg.rest_host, cfg.rest_port, cfg.use_tls),
      parser_(mapping) {}

void OKXRefDataAdapter::fetchSnapshot() {
    bpt::common::log::info(kLog(), "Starting snapshot fetch...");
    const uint64_t ts = now_ns();

    http::RestClient::Headers base_headers;
    if (cfg_.simulated)
        base_headers.emplace_back("x-simulated-trading", "1");

    // 1. Spot instruments
    try {
        auto body = client_.get("/api/v5/public/instruments?instType=SPOT", base_headers);
        for (auto& inst : parser_.parse_instruments(body, "SPOT", ts))
            registry_->add(inst);
    } catch (const std::exception& e) {
        bpt::common::log::error(kLog(), "Failed to fetch SPOT instruments: {}", e.what());
        throw;
    }

    // 2. Perpetual swap instruments
    try {
        auto body = client_.get("/api/v5/public/instruments?instType=SWAP", base_headers);
        for (auto& inst : parser_.parse_instruments(body, "SWAP", ts))
            registry_->add(inst);
    } catch (const std::exception& e) {
        bpt::common::log::error(kLog(), "Failed to fetch SWAP instruments: {}", e.what());
        // Non-fatal — continue without perp instruments
    }

    // 3. Fee schedule (signed private endpoint)
    const std::string fee_spot_target = "/api/v5/account/trade-fee?instType=SPOT";
    const std::string fee_swap_target = "/api/v5/account/trade-fee?instType=SWAP";
    try {
        auto headers = okx_auth_headers(api_key_, secret_key_, passphrase_, "GET", fee_spot_target, cfg_.simulated);
        auto body = client_.get(fee_spot_target, headers);
        for (auto& fs : parser_.parse_trade_fee(body, ts))
            if (on_fee_schedule)
                on_fee_schedule(fs);
    } catch (const std::exception& e) {
        bpt::common::log::warn(kLog(), "Failed to fetch SPOT trade-fee: {}", e.what());
    }
    try {
        auto headers = okx_auth_headers(api_key_, secret_key_, passphrase_, "GET", fee_swap_target, cfg_.simulated);
        auto body = client_.get(fee_swap_target, headers);
        for (auto& fs : parser_.parse_trade_fee(body, ts))
            if (on_fee_schedule)
                on_fee_schedule(fs);
    } catch (const std::exception& e) {
        bpt::common::log::warn(kLog(), "Failed to fetch SWAP trade-fee: {}", e.what());
    }

    ready_.store(true, std::memory_order_release);
    bpt::common::log::info(kLog(), "Snapshot complete. Registry has {} instruments.", registry_->getAll().size());
}

void OKXRefDataAdapter::fetchInstrumentListing() {
    bpt::common::log::info(kLog(), "Hourly instrument listing refresh...");
    const uint64_t ts = now_ns();

    http::RestClient::Headers base_headers;
    if (cfg_.simulated)
        base_headers.emplace_back("x-simulated-trading", "1");

    auto notify = [this, ts](refdata::Instrument& inst) {
        if (registry_->update_if_changed(inst) && on_instrument_delta)
            on_instrument_delta(inst, bpt::messages::DeltaUpdateType::MODIFY, ts);
    };

    try {
        auto body = client_.get("/api/v5/public/instruments?instType=SPOT", base_headers);
        for (auto& inst : parser_.parse_instruments(body, "SPOT", ts))
            notify(inst);
    } catch (const std::exception& e) {
        bpt::common::log::error(kLog(), "Hourly SPOT refresh failed: {}", e.what());
    }

    try {
        auto body = client_.get("/api/v5/public/instruments?instType=SWAP", base_headers);
        for (auto& inst : parser_.parse_instruments(body, "SWAP", ts))
            notify(inst);
    } catch (const std::exception& e) {
        bpt::common::log::error(kLog(), "Hourly SWAP refresh failed: {}", e.what());
    }
}

}  // namespace bpt::refdata::adapter
