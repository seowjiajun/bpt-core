#include "refdata/adapter/deribit/deribit_parser.h"

#include "refdata/refdata/types.h"

#include <messages/ExchangeId.h>
#include <messages/InstrumentType.h>

#include <cmath>
#include <nlohmann/json.hpp>
#include <bpt_common/logging.h>

using json = nlohmann::json;

namespace bpt::refdata::adapter {

namespace {

// Map bpt-refdata InstrumentType to SBE InstrumentType
bpt::messages::InstrumentType::Value to_sbe_inst_type(refdata::InstrumentType t) {
    switch (t) {
        case refdata::InstrumentType::SPOT:
            return bpt::messages::InstrumentType::SPOT;
        case refdata::InstrumentType::PERP:
            return bpt::messages::InstrumentType::PERPETUAL;
        case refdata::InstrumentType::FUTURE:
            return bpt::messages::InstrumentType::FUTURE;
        case refdata::InstrumentType::OPTION:
            return bpt::messages::InstrumentType::OPTION;
        default:
            return bpt::messages::InstrumentType::NULL_VALUE;
    }
}

// Determine instrument type from Deribit kind + settlement_period.
// kind=future with settlement_period=perpetual → PERP
// kind=future otherwise → FUTURE
// kind=option → OPTION
refdata::InstrumentType deribit_to_inst_type(const std::string& kind, const std::string& settlement_period) {
    if (kind == "option")
        return refdata::InstrumentType::OPTION;
    if (kind == "future") {
        if (settlement_period == "perpetual")
            return refdata::InstrumentType::PERP;
        return refdata::InstrumentType::FUTURE;
    }
    if (kind == "spot")
        return refdata::InstrumentType::SPOT;
    return refdata::InstrumentType::UNKNOWN;
}

}  // namespace

DeribitRefdataParser::DeribitRefdataParser(std::shared_ptr<mapping::InstrumentMappingLoader> mapping)
    : mapping_(std::move(mapping)) {}

std::vector<DeribitRefdataParser::InstrumentWithFee>
DeribitRefdataParser::parse_instruments(const std::string& body, uint64_t collected_ts) const {
    std::vector<InstrumentWithFee> out;

    auto j = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) {
        bpt::common::log::error("[DeribitRefData] get_instruments: invalid JSON");
        return out;
    }

    if (j.contains("error")) {
        bpt::common::log::error("[DeribitRefData] get_instruments error: {}",
                                j["error"].value("message", "unknown"));
        return out;
    }

    const auto& result = j["result"];
    if (!result.is_array()) {
        bpt::common::log::error("[DeribitRefData] get_instruments result is not an array");
        return out;
    }

    for (const auto& sym : result) {
        bool is_active = sym.value("is_active", false);
        if (!is_active)
            continue;

        std::string instrument_name = sym.value("instrument_name", "");
        if (instrument_name.empty())
            continue;

        std::string kind = sym.value("kind", "");
        std::string settlement_period = sym.value("settlement_period", "");

        refdata::InstrumentType itype = deribit_to_inst_type(kind, settlement_period);
        if (itype == refdata::InstrumentType::UNKNOWN)
            continue;

        // Deribit symbols are unique per type (BTC-PERPETUAL, BTC-28MAR25, ...).
        auto cid = mapping_->try_resolve_canonical_id(mapping::EXCHANGE_ID_DERIBIT, instrument_name);
        if (!cid)
            continue;

        InstrumentWithFee iwf;
        refdata::Instrument& inst = iwf.instrument;
        inst.inst_uid = mapping::make_inst_uid(*cid, mapping::EXCHANGE_ID_DERIBIT);
        inst.venue = "DERIBIT";
        inst.venue_symbol = instrument_name;
        inst.display_name = instrument_name;
        inst.base = sym.value("base_currency", "");
        inst.quote = sym.value("quote_currency", "");
        inst.inst_type = itype;
        inst.status = refdata::InstrumentStatus::ACTIVE;
        inst.version = collected_ts;

        // tick_size, min_trade_amount (lot_size), contract_size (contract_multiplier)
        inst.tick_size = sym.value("tick_size", 0.0);
        inst.lot_size = sym.value("min_trade_amount", 0.0);
        inst.contract_multiplier = sym.value("contract_size", 1.0);

        // Expiry for FUTURE and OPTION types (expiration_timestamp is ms)
        if (itype == refdata::InstrumentType::FUTURE || itype == refdata::InstrumentType::OPTION) {
            uint64_t exp_ms = sym.value("expiration_timestamp", static_cast<uint64_t>(0));
            if (exp_ms > 0)
                inst.expiry_timestamp = exp_ms * 1'000'000ULL;  // ms → ns
        }

        // Strike price for OPTIONS
        if (itype == refdata::InstrumentType::OPTION) {
            double strike = sym.value("strike", 0.0);
            if (strike > 0.0)
                inst.strike_price = strike;
        }

        // Fees come directly from instrument data (maker_commission, taker_commission).
        // These are decimal fractions (e.g. 0.0003 = 3 bps).
        double maker = sym.value("maker_commission", 0.0);
        double taker = sym.value("taker_commission", 0.0);

        refdata::FeeScheduleState& fs = iwf.fee;
        fs.exchange_id = bpt::messages::ExchangeId::DERIBIT;
        fs.instrument_id = inst.inst_uid;
        fs.instrument_type = to_sbe_inst_type(itype);
        fs.maker_fee_bps = static_cast<int16_t>(std::round(maker * 10000.0));
        fs.taker_fee_bps = static_cast<int16_t>(std::round(taker * 10000.0));
        fs.updated_ts = collected_ts;

        out.push_back(std::move(iwf));
    }

    return out;
}

}  // namespace bpt::refdata::adapter
