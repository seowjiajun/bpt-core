#include "refdata/adapter/hyperliquid/hyperliquid_refdata_decoder.h"

#include <messages/ExchangeId.h>
#include <messages/InstrumentType.h>

#include <bpt_common/logging.h>
#include <cmath>
#include <nlohmann/json.hpp>
#include <unordered_map>

using json = nlohmann::json;

namespace bpt::refdata::adapter {

HyperliquidRefdataDecoder::HyperliquidRefdataDecoder(std::shared_ptr<mapping::InstrumentMappingLoader> mapping)
    : mapping_(std::move(mapping)) {}

std::vector<model::Instrument> HyperliquidRefdataDecoder::parse_meta(const std::string& body,
                                                                       uint64_t collected_ts) const {
    auto j = json::parse(body);
    const auto& universe = j.value("universe", json::array());

    std::vector<model::Instrument> result;
    for (const auto& asset : universe) {
        std::string name = asset.value("name", "");
        if (name.empty())
            continue;

        auto cid = mapping_->try_resolve_canonical_id(mapping::EXCHANGE_ID_HYPERLIQUID, name);
        if (!cid)
            continue;

        model::Instrument inst;
        inst.venue = "HYPERLIQUID";
        inst.venue_symbol = name;
        inst.base = name;
        inst.quote = "USD";  // All HL perps are USD-quoted
        inst.inst_type = model::InstrumentType::PERP;
        inst.status = model::InstrumentStatus::ACTIVE;
        inst.version = collected_ts;
        inst.display_name = name + "-USD";
        inst.inst_uid = mapping::make_inst_uid(*cid, mapping::EXCHANGE_ID_HYPERLIQUID);
        inst.contract_multiplier = 1.0;

        // szDecimals defines the lot size precision: lot_size = 10^(-szDecimals)
        int sz_decimals = asset.value("szDecimals", 0);
        inst.lot_size = std::pow(10.0, -sz_decimals);

        // HL price convention: prices may have ≤ MAX_DECIMALS - szDecimals
        // decimal places (perps: MAX_DECIMALS=6, spot: 8). Tick = 10^-(that).
        // /info doesn't report tick directly; derive it. Without this, AS
        // quote rounding silently fails and the strategy posts zero orders.
        constexpr int kPerpMaxDecimals = 6;
        const int px_decimals = std::max(0, kPerpMaxDecimals - sz_decimals);
        inst.tick_size = std::pow(10.0, -px_decimals);

        result.push_back(std::move(inst));
    }

    bpt::common::log::info("[HyperliquidRefdataDecoder] Parsed {} perpetual instruments from /info meta",
                           result.size());
    return result;
}

std::vector<model::Instrument> HyperliquidRefdataDecoder::parse_spot_meta(const std::string& body,
                                                                            uint64_t collected_ts) const {
    auto j = json::parse(body);
    const auto& tokens = j.value("tokens", json::array());
    const auto& universe = j.value("universe", json::array());

    // Index tokens by their `index` field — universe entries reference
    // them by this index, not by array position (defensive: HL has
    // historically left gaps when delisting).
    struct TokenInfo {
        std::string name;
        int sz_decimals;
    };
    std::unordered_map<int, TokenInfo> token_by_idx;
    token_by_idx.reserve(tokens.size());
    for (const auto& t : tokens) {
        const int idx = t.value("index", -1);
        if (idx < 0)
            continue;
        token_by_idx.emplace(idx, TokenInfo{t.value("name", std::string{}), t.value("szDecimals", 0)});
    }

    std::vector<model::Instrument> result;
    for (const auto& pair : universe) {
        std::string name = pair.value("name", "");
        if (name.empty())
            continue;

        // Mapping gates which pairs flow through. Non-canonical "@N"
        // dev pairs that ops hasn't approved → no mapping entry → skip.
        auto cid = mapping_->try_resolve_canonical_id(mapping::EXCHANGE_ID_HYPERLIQUID, name);
        if (!cid)
            continue;

        const auto& token_pair = pair.value("tokens", json::array());
        if (token_pair.size() < 2)
            continue;
        const int base_idx = token_pair[0].get<int>();
        const int quote_idx = token_pair[1].get<int>();

        auto base_it = token_by_idx.find(base_idx);
        auto quote_it = token_by_idx.find(quote_idx);
        if (base_it == token_by_idx.end() || quote_it == token_by_idx.end())
            continue;
        if (base_it->second.name.empty() || quote_it->second.name.empty())
            continue;

        model::Instrument inst;
        inst.venue = "HYPERLIQUID";
        inst.venue_symbol = name;
        inst.base = base_it->second.name;
        inst.quote = quote_it->second.name;
        inst.inst_type = model::InstrumentType::SPOT;
        inst.status = model::InstrumentStatus::ACTIVE;
        inst.version = collected_ts;
        inst.display_name = name;
        inst.inst_uid = mapping::make_inst_uid(*cid, mapping::EXCHANGE_ID_HYPERLIQUID);
        inst.contract_multiplier = 1.0;

        // Spot: lot_size from base-token szDecimals, tick_size from the
        // spot price-decimal rule (MAX_DECIMALS=8 vs perps' 6).
        const int sz_decimals = base_it->second.sz_decimals;
        inst.lot_size = std::pow(10.0, -sz_decimals);

        constexpr int kSpotMaxDecimals = 8;
        const int px_decimals = std::max(0, kSpotMaxDecimals - sz_decimals);
        inst.tick_size = std::pow(10.0, -px_decimals);

        result.push_back(std::move(inst));
    }

    bpt::common::log::info("[HyperliquidRefdataDecoder] Parsed {} spot instruments from /info spotMeta", result.size());
    return result;
}

std::vector<model::FeeScheduleState> HyperliquidRefdataDecoder::parse_user_fees(const std::string& body,
                                                                                  uint64_t collected_ts) const {
    auto j = json::parse(body);
    const auto& sched = j.value("feeSchedule", json{});
    if (sched.is_null() || !sched.is_object()) {
        bpt::common::log::warn("[HyperliquidRefdataDecoder] userFees response missing feeSchedule");
        return {};
    }

    double maker = 0.0, taker = 0.0;
    if (auto it = sched.find("maker"); it != sched.end() && it->is_string())
        maker = std::stod(it->get<std::string>());
    if (auto it = sched.find("taker"); it != sched.end() && it->is_string())
        taker = std::stod(it->get<std::string>());

    model::FeeScheduleState fs;
    fs.exchange_id = bpt::messages::ExchangeId::HYPERLIQUID;
    fs.instrument_id = 0;  // 0 = applies to all instruments on Hyperliquid
    fs.instrument_type = bpt::messages::InstrumentType::PERPETUAL;
    fs.maker_fee_bps = static_cast<int16_t>(std::round(maker * 10000.0));
    fs.taker_fee_bps = static_cast<int16_t>(std::round(taker * 10000.0));
    fs.updated_ts = collected_ts;

    return {fs};
}

}  // namespace bpt::refdata::adapter
