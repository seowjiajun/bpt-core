#include "refdata/adapter/hyperliquid/hyperliquid_parser.h"

#include <messages/ExchangeId.h>
#include <messages/InstrumentType.h>

#include <cmath>
#include <nlohmann/json.hpp>
#include <bpt_common/logging.h>

using json = nlohmann::json;

namespace bpt::refdata::adapter {

HyperliquidParser::HyperliquidParser(std::shared_ptr<mapping::InstrumentMappingLoader> mapping)
    : mapping_(std::move(mapping)) {}

std::vector<refdata::Instrument> HyperliquidParser::parse_meta(const std::string& body, uint64_t collected_ts) const {
    auto j = json::parse(body);
    const auto& universe = j.value("universe", json::array());

    std::vector<refdata::Instrument> result;
    for (const auto& asset : universe) {
        std::string name = asset.value("name", "");
        if (name.empty())
            continue;

        auto cid = mapping_->try_resolve_canonical_id(mapping::EXCHANGE_ID_HYPERLIQUID, name);
        if (!cid)
            continue;

        refdata::Instrument inst;
        inst.venue = "HYPERLIQUID";
        inst.venue_symbol = name;
        inst.base = name;
        inst.quote = "USD";  // All HL perps are USD-quoted
        inst.inst_type = refdata::InstrumentType::PERP;
        inst.status = refdata::InstrumentStatus::ACTIVE;
        inst.version = collected_ts;
        inst.display_name = name + "-USD";
        inst.inst_uid = mapping::make_inst_uid(*cid, mapping::EXCHANGE_ID_HYPERLIQUID);
        inst.contract_multiplier = 1.0;

        // szDecimals defines the lot size precision: lot_size = 10^(-szDecimals)
        int sz_decimals = asset.value("szDecimals", 0);
        inst.lot_size = std::pow(10.0, -sz_decimals);
        inst.tick_size = 0.0;  // not in meta; updated from assetCtxs

        result.push_back(std::move(inst));
    }

    bpt::common::log::info("[HyperliquidParser] Parsed {} perpetual instruments from /info meta", result.size());
    return result;
}

std::vector<refdata::FeeScheduleState> HyperliquidParser::parse_user_fees(const std::string& body,
                                                                          uint64_t collected_ts) const {
    auto j = json::parse(body);
    const auto& sched = j.value("feeSchedule", json{});
    if (sched.is_null() || !sched.is_object()) {
        bpt::common::log::warn("[HyperliquidParser] userFees response missing feeSchedule");
        return {};
    }

    double maker = 0.0, taker = 0.0;
    if (auto it = sched.find("maker"); it != sched.end() && it->is_string())
        maker = std::stod(it->get<std::string>());
    if (auto it = sched.find("taker"); it != sched.end() && it->is_string())
        taker = std::stod(it->get<std::string>());

    refdata::FeeScheduleState fs;
    fs.exchange_id = bpt::messages::ExchangeId::HYPERLIQUID;
    fs.instrument_id = 0;  // 0 = applies to all instruments on Hyperliquid
    fs.instrument_type = bpt::messages::InstrumentType::PERPETUAL;
    fs.maker_fee_bps = static_cast<int16_t>(std::round(maker * 10000.0));
    fs.taker_fee_bps = static_cast<int16_t>(std::round(taker * 10000.0));
    fs.updated_ts = collected_ts;

    return {fs};
}

}  // namespace bpt::refdata::adapter
