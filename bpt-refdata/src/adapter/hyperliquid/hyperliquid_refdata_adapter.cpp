#include "refdata/adapter/hyperliquid/hyperliquid_refdata_adapter.h"

#include <messages/DeltaUpdateType.h>

#include <nlohmann/json.hpp>
#include <bpt_common/util/tsc_clock.h>

namespace bpt::refdata::adapter {

namespace {

uint64_t now_ns() {
    return bpt::common::util::TscClock::now_epoch_ns();
}

}  // namespace

HyperliquidRefDataAdapter::HyperliquidRefDataAdapter(const config::AdapterConfig& cfg,
                                                     const ExchangeCredentials& creds,
                                                     std::shared_ptr<registry::InstrumentRegistry> registry,
                                                     std::shared_ptr<mapping::InstrumentMappingLoader> mapping)
    : cfg_(cfg),
      registry_(std::move(registry)),
      wallet_address_(creds.wallet_address),
      client_(cfg.rest_host.empty() ? "api.hyperliquid.xyz" : cfg.rest_host, cfg.rest_port, cfg.use_tls),
      parser_(mapping) {}

void HyperliquidRefDataAdapter::fetchSnapshot() {
    bpt::common::log::info("[HyperliquidRefData] Starting snapshot fetch...");
    const uint64_t ts = now_ns();

    // 1. Meta — instrument listing (all perps)
    try {
        auto body = client_.post("/info", R"({"type":"meta"})");
        for (auto& inst : parser_.parse_meta(body, ts))
            registry_->add(inst);
    } catch (const std::exception& e) {
        bpt::common::log::error("[HyperliquidRefData] Failed to fetch meta: {}", e.what());
        throw;
    }

    // 2. User fees (requires wallet address)
    if (!wallet_address_.empty()) {
        try {
            nlohmann::json fee_req = {{"type", "userFees"}, {"user", wallet_address_}};
            auto body = client_.post("/info", fee_req.dump());
            for (auto& fs : parser_.parse_user_fees(body, ts))
                if (on_fee_schedule)
                    on_fee_schedule(fs);
        } catch (const std::exception& e) {
            bpt::common::log::warn("[HyperliquidRefData] Failed to fetch userFees: {}", e.what());
        }
    } else {
        bpt::common::log::warn("[HyperliquidRefData] No wallet address configured — skipping userFees");
    }

    ready_.store(true, std::memory_order_release);
    bpt::common::log::info("[HyperliquidRefData] Snapshot complete. Registry has {} instruments.", registry_->getAll().size());
}

void HyperliquidRefDataAdapter::fetchInstrumentListing() {
    bpt::common::log::info("[HyperliquidRefData] Hourly instrument listing refresh...");
    const uint64_t ts = now_ns();

    try {
        auto body = client_.post("/info", R"({"type":"meta"})");
        for (auto& inst : parser_.parse_meta(body, ts)) {
            if (registry_->update_if_changed(inst) && on_instrument_delta)
                on_instrument_delta(inst, bpt::messages::DeltaUpdateType::MODIFY, ts);
        }
    } catch (const std::exception& e) {
        bpt::common::log::error("[HyperliquidRefData] Hourly meta refresh failed: {}", e.what());
    }
}

}  // namespace bpt::refdata::adapter
