#pragma once

/// \file
/// \brief Hyperliquid REST refdata response decoder (JSON → refdata structs).

#include "refdata/mapping/instrument_mapping_loader.h"
#include "refdata/model/fee_schedule.h"
#include "refdata/model/instrument.h"

#include <memory>
#include <string>
#include <vector>

namespace bpt::refdata::adapter {

/// \brief Pure JSON decoder for Hyperliquid REST refdata responses.
///
/// No network I/O, no side effects — suitable for unit testing with
/// fixture data.
class HyperliquidRefdataDecoder {
public:
    explicit HyperliquidRefdataDecoder(std::shared_ptr<mapping::InstrumentMappingLoader> mapping);

    /// \brief Decode `POST /info {"type":"meta"}` body (perpetuals).
    std::vector<model::Instrument> parse_meta(const std::string& body, uint64_t collected_ts) const;

    /// \brief Decode `POST /info {"type":"spotMeta"}` body (spot pairs).
    ///
    /// HL spot universe entries reference two tokens (base, quote) by
    /// position in the `tokens[]` array. Tick precision follows the
    /// spot convention (MAX_DECIMALS=8, vs perps' 6). Pairs whose
    /// venue_symbol (HL `name` field — e.g. "PURR/USDC") isn't in the
    /// mapping JSON are silently skipped — same gating as perps.
    std::vector<model::Instrument> parse_spot_meta(const std::string& body, uint64_t collected_ts) const;

    /// \brief Decode `POST /info {"type":"userFees","user":"<wallet_address>"}` body.
    std::vector<model::FeeScheduleState> parse_user_fees(const std::string& body, uint64_t collected_ts) const;

private:
    std::shared_ptr<mapping::InstrumentMappingLoader> mapping_;
};

}  // namespace bpt::refdata::adapter
