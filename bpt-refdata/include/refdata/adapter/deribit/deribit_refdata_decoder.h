#pragma once

/// \file
/// \brief Deribit `public/get_instruments` decoder (JSON → refdata structs).

#include "refdata/mapping/instrument_mapping_loader.h"
#include "refdata/model/fee_schedule.h"
#include "refdata/model/instrument.h"

#include <memory>
#include <string>
#include <vector>

namespace bpt::refdata::adapter {

/// \brief Pure JSON decoder for Deribit `public/get_instruments` responses.
///
/// No network I/O, no side effects — suitable for unit testing with
/// fixture data.
///
/// Deribit's `get_instruments` response embeds per-instrument
/// maker/taker fees (unlike Binance/OKX, where fees come from a
/// separate endpoint), so parse returns both in a single pass — the
/// adapter pushes each half to its respective registry / callback.
class DeribitRefdataDecoder {
public:
    struct InstrumentWithFee {
        model::Instrument instrument;
        model::FeeScheduleState fee;
    };

    explicit DeribitRefdataDecoder(std::shared_ptr<mapping::InstrumentMappingLoader> mapping);

    /// \brief Decode `POST /api/v2 public/get_instruments` body.
    ///
    /// Returns active instruments whose canonical ID resolves via the
    /// mapping loader.
    std::vector<InstrumentWithFee> parse_instruments(const std::string& body, uint64_t collected_ts) const;

private:
    std::shared_ptr<mapping::InstrumentMappingLoader> mapping_;
};

}  // namespace bpt::refdata::adapter
