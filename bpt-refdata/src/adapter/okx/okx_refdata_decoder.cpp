#include "refdata/adapter/okx/okx_refdata_decoder.h"

#include "refdata/refdata/types.h"

#include <messages/ExchangeId.h>
#include <messages/InstrumentType.h>

#include <bpt_common/logging.h>
#include <cmath>
#include <nlohmann/json.hpp>
#include <stdexcept>

using json = nlohmann::json;

namespace bpt::refdata::adapter {

namespace {

refdata::InstrumentType okx_to_inst_type(const std::string& inst_type) {
    if (inst_type == "SPOT")
        return refdata::InstrumentType::SPOT;
    if (inst_type == "SWAP")
        return refdata::InstrumentType::PERP;
    if (inst_type == "FUTURES")
        return refdata::InstrumentType::FUTURE;
    if (inst_type == "OPTION")
        return refdata::InstrumentType::OPTION;
    return refdata::InstrumentType::UNKNOWN;
}

refdata::InstrumentStatus okx_to_status(const std::string& state) {
    if (state == "live")
        return refdata::InstrumentStatus::ACTIVE;
    if (state == "suspend")
        return refdata::InstrumentStatus::HALTED;
    return refdata::InstrumentStatus::DELISTED;
}

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

}  // namespace

OKXRefdataDecoder::OKXRefdataDecoder(std::shared_ptr<mapping::InstrumentMappingLoader> mapping)
    : mapping_(std::move(mapping)) {}

std::vector<refdata::Instrument> OKXRefdataDecoder::parse_instruments(const std::string& body,
                                                                      const std::string& inst_type,
                                                                      uint64_t collected_ts) const {
    auto j = json::parse(body);
    if (j.value("code", "") != "0") {
        bpt::common::log::error("[OKXRefdataDecoder] instruments API error code={} msg={}",
                                j.value("code", "?"),
                                j.value("msg", "?"));
        return {};
    }

    const auto& data = j["data"];
    std::vector<refdata::Instrument> result;

    for (const auto& sym : data) {
        if (sym.value("state", "") != "live")
            continue;

        std::string venue_symbol = sym.value("instId", "");
        if (venue_symbol.empty())
            continue;

        std::string base = sym.value("baseCcy", "");
        std::string quote = sym.value("quoteCcy", "");

        // For SWAP/FUTURES, baseCcy/quoteCcy may be empty — derive from instId.
        if (base.empty()) {
            auto dash = venue_symbol.find('-');
            if (dash != std::string::npos)
                base = venue_symbol.substr(0, dash);
        }
        if (quote.empty()) {
            auto p = venue_symbol.find('-');
            if (p != std::string::npos) {
                auto p2 = venue_symbol.find('-', p + 1);
                quote = (p2 != std::string::npos) ? venue_symbol.substr(p + 1, p2 - p - 1) : venue_symbol.substr(p + 1);
            }
        }

        auto cid = mapping_->try_resolve_canonical_id(mapping::EXCHANGE_ID_OKX, venue_symbol);
        if (!cid)
            continue;

        refdata::Instrument inst;
        inst.venue = "OKX";
        inst.venue_symbol = venue_symbol;
        inst.base = base;
        inst.quote = quote;
        inst.inst_type = okx_to_inst_type(inst_type);
        inst.status = okx_to_status(sym.value("state", ""));
        inst.version = collected_ts;
        inst.display_name = venue_symbol;
        inst.inst_uid = mapping::make_inst_uid(*cid, mapping::EXCHANGE_ID_OKX);

        inst.tick_size = 0.0;
        double lot_sz_raw = 0.0;
        if (auto it = sym.find("tickSz"); it != sym.end() && it->is_string())
            inst.tick_size = std::stod(it->get<std::string>());
        if (auto it = sym.find("lotSz"); it != sym.end() && it->is_string())
            lot_sz_raw = std::stod(it->get<std::string>());

        inst.contract_multiplier = 1.0;
        if (auto it = sym.find("ctVal"); it != sym.end() && it->is_string()) {
            auto s = it->get<std::string>();
            if (!s.empty())
                inst.contract_multiplier = std::stod(s);
        }
        inst.lot_size = lot_sz_raw * inst.contract_multiplier;

        if (inst.inst_type == refdata::InstrumentType::FUTURE) {
            if (auto it = sym.find("expTime"); it != sym.end() && it->is_string()) {
                auto s = it->get<std::string>();
                if (!s.empty()) {
                    uint64_t exp_ms = std::stoull(s);
                    if (exp_ms > 0)
                        inst.expiry_timestamp = exp_ms * 1'000'000ULL;
                }
            }
        }

        result.push_back(std::move(inst));
    }

    bpt::common::log::info("[OKXRefdataDecoder] Parsed {} {} instruments", result.size(), inst_type);
    return result;
}

std::vector<refdata::FeeScheduleState> OKXRefdataDecoder::parse_trade_fee(const std::string& body,
                                                                          uint64_t collected_ts) const {
    auto j = json::parse(body);
    if (j.value("code", "") != "0") {
        bpt::common::log::warn("[OKXRefdataDecoder] trade-fee API error code={}", j.value("code", "?"));
        return {};
    }

    std::vector<refdata::FeeScheduleState> result;
    for (const auto& entry : j["data"]) {
        std::string inst_type_str = entry.value("instType", "");
        refdata::InstrumentType itype = okx_to_inst_type(inst_type_str);

        double maker = 0.0, taker = 0.0;
        if (auto it = entry.find("maker"); it != entry.end() && it->is_string()) {
            auto s = it->get<std::string>();
            if (!s.empty())
                maker = std::stod(s);
        }
        if (auto it = entry.find("taker"); it != entry.end() && it->is_string()) {
            auto s = it->get<std::string>();
            if (!s.empty())
                taker = std::stod(s);
        }

        refdata::FeeScheduleState fs;
        fs.exchange_id = bpt::messages::ExchangeId::OKX;
        fs.instrument_id = 0;  // 0 = applies to all instruments of this type on OKX
        fs.instrument_type = to_sbe_inst_type(itype);
        fs.maker_fee_bps = static_cast<int16_t>(std::round(maker * 10000.0));
        fs.taker_fee_bps = static_cast<int16_t>(std::round(taker * 10000.0));
        fs.updated_ts = collected_ts;

        result.push_back(fs);
    }

    return result;
}

}  // namespace bpt::refdata::adapter
