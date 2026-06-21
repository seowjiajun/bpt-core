#pragma once

/// \file
/// \brief OKX REST refdata response decoder (JSON → refdata structs).

#include "refdata/mapping/instrument_mapping_loader.h"
#include "refdata/model/fee_schedule.h"
#include "refdata/model/instrument.h"

#include <memory>
#include <string>
#include <vector>

namespace bpt::refdata::adapter {

/// \brief Pure JSON decoder for OKX REST refdata responses.
///
/// No network I/O, no side effects — suitable for unit testing with
/// fixture data.
class OKXRefdataDecoder {
public:
    explicit OKXRefdataDecoder(std::shared_ptr<mapping::InstrumentMappingLoader> mapping);

    /// \brief Decode `GET /api/v5/public/instruments?instType=SPOT|SWAP|FUTURES` body.
    std::vector<model::Instrument> parse_instruments(const std::string& body,
                                                     const std::string& inst_type,
                                                     uint64_t collected_ts) const;

    /// \brief Decode `GET /api/v5/account/trade-fee?instType=SPOT|SWAP` body.
    std::vector<model::FeeScheduleState> parse_trade_fee(const std::string& body, uint64_t collected_ts) const;

private:
    std::shared_ptr<mapping::InstrumentMappingLoader> mapping_;
};

}  // namespace bpt::refdata::adapter
