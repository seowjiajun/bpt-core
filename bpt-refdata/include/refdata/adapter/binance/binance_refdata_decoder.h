#pragma once

/// \file
/// \brief Binance REST refdata response decoder (JSON → refdata structs).

#include "refdata/mapping/instrument_mapping_loader.h"
#include "refdata/model/funding_rate.h"
#include "refdata/model/instrument.h"

#include <memory>
#include <string>
#include <vector>

namespace bpt::refdata::adapter {

/// \brief Pure JSON decoder for Binance REST refdata responses.
///
/// No network I/O, no side effects — suitable for unit testing with
/// fixture data.
class BinanceRefdataDecoder {
public:
    explicit BinanceRefdataDecoder(std::shared_ptr<mapping::InstrumentMappingLoader> mapping);

    /// \brief Decode `GET /api/v3/exchangeInfo` body.
    std::vector<model::Instrument> parse_spot_exchange_info(const std::string& body, uint64_t collected_ts) const;

    /// \brief Decode `GET /fapi/v1/exchangeInfo` body.
    std::vector<model::Instrument> parse_futures_exchange_info(const std::string& body, uint64_t collected_ts) const;

    /// \brief Decode `GET /sapi/v1/asset/tradeFee` body.
    std::vector<model::FeeScheduleState> parse_trade_fee(const std::string& body, uint64_t collected_ts) const;

private:
    std::shared_ptr<mapping::InstrumentMappingLoader> mapping_;
};

}  // namespace bpt::refdata::adapter
