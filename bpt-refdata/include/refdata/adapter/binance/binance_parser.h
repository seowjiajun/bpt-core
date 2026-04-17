#pragma once

#include "refdata/mapping/instrument_mapping_loader.h"
#include "refdata/refdata/funding_rate.h"
#include "refdata/refdata/instrument.h"

#include <memory>
#include <string>
#include <vector>

namespace bpt::refdata::adapter {

// Pure JSON parser for Binance REST refdata responses.
// No network I/O, no side effects — suitable for unit testing with fixture data.
class BinanceParser {
public:
    explicit BinanceParser(std::shared_ptr<mapping::InstrumentMappingLoader> mapping);

    // GET /api/v3/exchangeInfo
    std::vector<refdata::Instrument> parse_spot_exchange_info(const std::string& body, uint64_t collected_ts) const;

    // GET /fapi/v1/exchangeInfo
    std::vector<refdata::Instrument> parse_futures_exchange_info(const std::string& body, uint64_t collected_ts) const;

    // GET /sapi/v1/asset/tradeFee
    std::vector<refdata::FeeScheduleState> parse_trade_fee(const std::string& body, uint64_t collected_ts) const;

private:
    std::shared_ptr<mapping::InstrumentMappingLoader> mapping_;
};

}  // namespace bpt::refdata::adapter
