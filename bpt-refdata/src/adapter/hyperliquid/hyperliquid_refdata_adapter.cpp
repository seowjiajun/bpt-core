#include "refdata/adapter/hyperliquid/hyperliquid_refdata_adapter.h"

#include <messages/DeltaUpdateType.h>

#include <bpt_common/util/tsc_clock.h>
#include <nlohmann/json.hpp>

namespace bpt::refdata::adapter {

namespace {

uint64_t now_ns() {
    return bpt::common::util::TscClock::now_epoch_ns();
}

}  // namespace

HyperliquidRefDataAdapter::HyperliquidRefDataAdapter(const config::AdapterConfig& cfg,
                                                     const ExchangeCredentials& creds,
                                                     std::shared_ptr<registry::InstrumentRegistry> registry,
                                                     std::shared_ptr<mapping::InstrumentMappingLoader> mapping,
                                                     std::shared_ptr<http::RestClient> client)
    : cfg_(cfg),
      registry_(std::move(registry)),
      wallet_address_(creds.wallet_address),
      client_(std::move(client)),
      decoder_(mapping) {}

void HyperliquidRefDataAdapter::fetchSnapshot() {
    bpt::common::log::info("[HyperliquidRefData] Starting snapshot fetch...");
    const uint64_t ts = now_ns();

    // 1. Meta — instrument listing (all perps)
    try {
        auto body = client_->post("/info", R"({"type":"meta"})");
        for (auto& inst : decoder_.parse_meta(body, ts))
            registry_->add(inst);
    } catch (const std::exception& e) {
        bpt::common::log::error("[HyperliquidRefData] Failed to fetch meta: {}", e.what());
        throw;
    }

    // 2. spotMeta — HL spot pairs. Optional: testnet may have none we
    // care about, mainnet has a handful. Mapping JSON gates which pairs
    // actually flow through. A failure here doesn't abort the snapshot —
    // perps are the primary product, spot is additive.
    try {
        auto body = client_->post("/info", R"({"type":"spotMeta"})");
        for (auto& inst : decoder_.parse_spot_meta(body, ts))
            registry_->add(inst);
    } catch (const std::exception& e) {
        bpt::common::log::warn("[HyperliquidRefData] Failed to fetch spotMeta: {}", e.what());
    }

    // 3. User fees (requires wallet address)
    if (!wallet_address_.empty()) {
        try {
            nlohmann::json fee_req = {{"type", "userFees"}, {"user", wallet_address_}};
            auto body = client_->post("/info", fee_req.dump());
            for (auto& fs : decoder_.parse_user_fees(body, ts))
                if (on_fee_schedule)
                    on_fee_schedule(fs);
        } catch (const std::exception& e) {
            bpt::common::log::warn("[HyperliquidRefData] Failed to fetch userFees: {}", e.what());
        }
    } else {
        bpt::common::log::warn("[HyperliquidRefData] No wallet address configured — skipping userFees");
    }

    ready_.store(true, std::memory_order_release);
    bpt::common::log::info("[HyperliquidRefData] Snapshot complete. Registry has {} instruments.",
                           registry_->getAll().size());
}

void HyperliquidRefDataAdapter::fetchInstrumentListing() {
    bpt::common::log::info("[HyperliquidRefData] Hourly instrument listing refresh...");
    const uint64_t ts = now_ns();

    try {
        auto body = client_->post("/info", R"({"type":"meta"})");
        for (auto& inst : decoder_.parse_meta(body, ts)) {
            if (registry_->update_if_changed(inst) && on_instrument_delta)
                on_instrument_delta(inst, bpt::messages::DeltaUpdateType::MODIFY, ts);
        }
    } catch (const std::exception& e) {
        bpt::common::log::error("[HyperliquidRefData] Hourly meta refresh failed: {}", e.what());
    }

    try {
        auto body = client_->post("/info", R"({"type":"spotMeta"})");
        for (auto& inst : decoder_.parse_spot_meta(body, ts)) {
            if (registry_->update_if_changed(inst) && on_instrument_delta)
                on_instrument_delta(inst, bpt::messages::DeltaUpdateType::MODIFY, ts);
        }
    } catch (const std::exception& e) {
        bpt::common::log::warn("[HyperliquidRefData] Hourly spotMeta refresh failed: {}", e.what());
    }
}

}  // namespace bpt::refdata::adapter
