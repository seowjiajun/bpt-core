#include "refdata/adapter/deribit/deribit_refdata_adapter.h"

#include "refdata/adapter/deribit/deribit_refdata_encoder.h"

#include <messages/DeltaUpdateType.h>

#include <atomic>
#include <bpt_common/logging.h>
#include <bpt_common/util/tsc_clock.h>

namespace bpt::refdata::adapter {

namespace {

uint64_t now_ns() {
    return bpt::common::util::TscClock::now_epoch_ns();
}

// Atomic JSON-RPC request id generator — shared across adapter
// instances so ids are globally unique per process.
std::atomic<uint64_t> g_jsonrpc_id{1};

}  // namespace

DeribitRefDataAdapter::DeribitRefDataAdapter(const config::AdapterConfig& cfg,
                                             const ExchangeCredentials& creds,
                                             std::shared_ptr<registry::InstrumentRegistry> registry,
                                             std::shared_ptr<mapping::InstrumentMappingLoader> mapping,
                                             std::shared_ptr<http::RestClient> rest_client)
    : cfg_(cfg),
      registry_(std::move(registry)),
      client_id_(creds.client_id),
      client_secret_(creds.client_secret),
      rest_client_(std::move(rest_client)),
      parser_(std::move(mapping)) {
    if (client_id_.empty() || client_secret_.empty()) {
        bpt::common::log::warn(
            "[DeribitRefData] Deribit credentials not set — "
            "private endpoints (fee tiers) will not be available; "
            "using per-instrument fees from get_instruments instead.");
    }
}

void DeribitRefDataAdapter::ingest_instruments(const std::string& currency,
                                                const std::string& kind,
                                                uint64_t collected_ts,
                                                bool notify_deltas) {
    const auto params = deribit::build_get_instruments_params(currency, kind);
    const auto body = deribit::build_jsonrpc_body(
        g_jsonrpc_id.fetch_add(1, std::memory_order_relaxed),
        "public/get_instruments",
        params);

    std::string response;
    try {
        response = rest_client_->post("/api/v2", body);
    } catch (const std::exception& e) {
        bpt::common::log::error("[DeribitRefData] get_instruments {} {} failed: {}",
                                currency, kind, e.what());
        return;
    }

    int loaded = 0;
    for (auto& iwf : parser_.parse_instruments(response, collected_ts)) {
        const bool changed = registry_->update_if_changed(iwf.instrument);
        if (changed && notify_deltas && on_instrument_delta)
            on_instrument_delta(iwf.instrument, bpt::messages::DeltaUpdateType::MODIFY, collected_ts);
        if (on_fee_schedule)
            on_fee_schedule(iwf.fee);
        ++loaded;
    }
    bpt::common::log::info("[DeribitRefData] Loaded {} active instruments from get_instruments ({} {})",
                           loaded, currency, kind);
}

void DeribitRefDataAdapter::fetchSnapshot() {
    bpt::common::log::info("[DeribitRefData] Starting snapshot fetch...");

    const uint64_t ts = now_ns();
    for (const auto& currency : {"BTC", "ETH"})
        for (const auto& kind : {"future", "option"})
            ingest_instruments(currency, kind, ts, /*notify_deltas=*/false);

    ready_.store(true, std::memory_order_release);
    bpt::common::log::info("[DeribitRefData] Snapshot complete. Registry has {} instruments.", registry_->count());
}

void DeribitRefDataAdapter::fetchInstrumentListing() {
    bpt::common::log::info("[DeribitRefData] Hourly instrument listing refresh...");

    const uint64_t ts = now_ns();
    for (const auto& currency : {"BTC", "ETH"})
        for (const auto& kind : {"future", "option"})
            ingest_instruments(currency, kind, ts, /*notify_deltas=*/true);
}

}  // namespace bpt::refdata::adapter
