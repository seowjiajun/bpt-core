#pragma once

#include "refdata/mapping/instrument_mapping_loader.h"
#include "refdata/refdata/funding_rate.h"
#include "refdata/refdata/instrument.h"

#include <memory>
#include <string>
#include <vector>

namespace bpt::refdata::adapter {

// Pure JSON parser for OKX REST refdata responses.
// No network I/O, no side effects — suitable for unit testing with fixture data.
class OKXParser {
public:
    explicit OKXParser(std::shared_ptr<mapping::InstrumentMappingLoader> mapping);

    // GET /api/v5/public/instruments?instType=SPOT|SWAP|FUTURES
    std::vector<refdata::Instrument> parse_instruments(const std::string& body,
                                                       const std::string& inst_type,
                                                       uint64_t collected_ts) const;

    // GET /api/v5/account/trade-fee?instType=SPOT|SWAP
    std::vector<refdata::FeeScheduleState> parse_trade_fee(const std::string& body, uint64_t collected_ts) const;

private:
    std::shared_ptr<mapping::InstrumentMappingLoader> mapping_;
};

}  // namespace bpt::refdata::adapter
