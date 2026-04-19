#include "refdata/adapter/binance/binance_parser.h"

#include "refdata/refdata/types.h"

#include <messages/ExchangeId.h>
#include <messages/InstrumentType.h>

#include <cmath>
#include <nlohmann/json.hpp>
#include <bpt_common/logging.h>

using json = nlohmann::json;

namespace bpt::refdata::adapter {

namespace {

refdata::InstrumentType binance_to_inst_type(const std::string& contract_type) {
    if (contract_type == "PERPETUAL")
        return refdata::InstrumentType::PERP;
    if (contract_type == "CURRENT_QUARTER" || contract_type == "NEXT_QUARTER" || contract_type == "CURRENT_MONTH" ||
        contract_type == "NEXT_MONTH")
        return refdata::InstrumentType::FUTURE;
    return refdata::InstrumentType::UNKNOWN;
}

refdata::InstrumentStatus binance_to_status(const std::string& s) {
    if (s == "TRADING")
        return refdata::InstrumentStatus::ACTIVE;
    if (s == "BREAK")
        return refdata::InstrumentStatus::HALTED;
    return refdata::InstrumentStatus::DELISTED;
}

void parse_filters(const json& filters, double& tick_size, double& lot_size, double& min_notional) {
    tick_size = 0.0;
    lot_size = 0.0;
    min_notional = 0.0;
    for (const auto& f : filters) {
        auto t = f.value("filterType", std::string{});
        if (t == "PRICE_FILTER")
            tick_size = std::stod(f.value("tickSize", "0"));
        if (t == "LOT_SIZE")
            lot_size = std::stod(f.value("stepSize", "0"));
        if (t == "MIN_NOTIONAL" || t == "NOTIONAL")
            min_notional = std::stod(f.value("minNotional", "0"));
    }
}

}  // namespace

BinanceParser::BinanceParser(std::shared_ptr<mapping::InstrumentMappingLoader> mapping)
    : mapping_(std::move(mapping)) {}

std::vector<refdata::Instrument> BinanceParser::parse_spot_exchange_info(const std::string& body,
                                                                         uint64_t collected_ts) const {
    auto j = json::parse(body);
    std::vector<refdata::Instrument> result;

    for (const auto& sym : j["symbols"]) {
        if (sym.value("status", "") != "TRADING")
            continue;

        refdata::Instrument inst;
        inst.venue = "BINANCE";
        inst.venue_symbol = sym.value("symbol", "");
        inst.base = sym.value("baseAsset", "");
        inst.quote = sym.value("quoteAsset", "");
        inst.inst_type = refdata::InstrumentType::SPOT;
        inst.status = refdata::InstrumentStatus::ACTIVE;
        inst.version = collected_ts;
        inst.contract_multiplier = 1.0;
        inst.display_name = inst.venue_symbol;

        // Binance reuses the same symbol for spot and perp — append _SPOT to
        // match the instrument_mapping.json forward key convention.
        auto cid = mapping_->try_resolve_canonical_id(mapping::EXCHANGE_ID_BINANCE, inst.venue_symbol + "_SPOT");
        if (!cid)
            continue;
        inst.inst_uid = mapping::make_inst_uid(*cid, mapping::EXCHANGE_ID_BINANCE);

        double tick{}, lot{}, min_notional{};
        parse_filters(sym.value("filters", json::array()), tick, lot, min_notional);
        inst.tick_size = tick;
        inst.lot_size = lot;

        result.push_back(std::move(inst));
    }

    bpt::common::log::info("[BinanceParser] Parsed {} spot instruments from exchangeInfo", result.size());
    return result;
}

std::vector<refdata::Instrument> BinanceParser::parse_futures_exchange_info(const std::string& body,
                                                                            uint64_t collected_ts) const {
    auto j = json::parse(body);
    std::vector<refdata::Instrument> result;

    for (const auto& sym : j["symbols"]) {
        auto status = sym.value("status", "");
        if (status != "TRADING" && status != "DELIVERING")
            continue;

        refdata::Instrument inst;
        inst.venue = "BINANCE";
        inst.venue_symbol = sym.value("symbol", "");
        inst.base = sym.value("baseAsset", "");
        inst.quote = sym.value("quoteAsset", "");
        inst.inst_type = binance_to_inst_type(sym.value("contractType", ""));
        inst.status = binance_to_status(status);
        inst.contract_multiplier = sym.value("contractSize", 1.0);
        inst.version = collected_ts;
        inst.display_name = inst.venue_symbol;

        auto cid = mapping_->try_resolve_canonical_id(mapping::EXCHANGE_ID_BINANCE, inst.venue_symbol);
        if (!cid)
            continue;
        inst.inst_uid = mapping::make_inst_uid(*cid, mapping::EXCHANGE_ID_BINANCE);

        if (auto it = sym.find("deliveryDate"); it != sym.end() && it->is_number()) {
            uint64_t delivery_ms = it->get<uint64_t>();
            if (delivery_ms > 0)
                inst.expiry_timestamp = delivery_ms * 1'000'000ULL;
        }

        double tick{}, lot{}, min_notional{};
        parse_filters(sym.value("filters", json::array()), tick, lot, min_notional);
        inst.tick_size = tick;
        inst.lot_size = lot;

        result.push_back(std::move(inst));
    }

    bpt::common::log::info("[BinanceParser] Parsed {} futures/perp instruments from fapi exchangeInfo", result.size());
    return result;
}

std::vector<refdata::FeeScheduleState> BinanceParser::parse_trade_fee(const std::string& body,
                                                                      uint64_t collected_ts) const {
    auto j = json::parse(body);
    std::vector<refdata::FeeScheduleState> result;
    if (!j.is_array())
        return result;

    for (const auto& entry : j) {
        auto symbol = entry.value("symbol", "");
        if (symbol.empty())
            continue;

        auto cid = mapping_->try_resolve_canonical_id(mapping::EXCHANGE_ID_BINANCE, symbol + "_SPOT");
        if (!cid)
            continue;

        double maker = std::stod(entry.value("makerCommission", "0.001"));
        double taker = std::stod(entry.value("takerCommission", "0.001"));

        refdata::FeeScheduleState fs;
        fs.exchange_id = bpt::messages::ExchangeId::BINANCE;
        fs.instrument_id = mapping::make_inst_uid(*cid, mapping::EXCHANGE_ID_BINANCE);
        fs.instrument_type = bpt::messages::InstrumentType::SPOT;
        fs.maker_fee_bps = static_cast<int16_t>(std::round(maker * 10000.0));
        fs.taker_fee_bps = static_cast<int16_t>(std::round(taker * 10000.0));
        fs.updated_ts = collected_ts;

        result.push_back(fs);
    }

    return result;
}

}  // namespace bpt::refdata::adapter
