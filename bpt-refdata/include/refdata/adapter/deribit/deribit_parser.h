#pragma once

#include "refdata/mapping/instrument_mapping_loader.h"
#include "refdata/refdata/funding_rate.h"
#include "refdata/refdata/instrument.h"

#include <memory>
#include <string>
#include <vector>

namespace bpt::refdata::adapter {

// Pure JSON parser for Deribit public/get_instruments responses.
// No network I/O, no side effects — suitable for unit testing with
// fixture data.
//
// Deribit's get_instruments response embeds per-instrument maker/taker
// fees (unlike Binance/OKX, where fees come from a separate endpoint),
// so parse returns both in a single pass — the adapter pushes each
// half to its respective registry / callback.
class DeribitRefdataParser {
public:
    struct InstrumentWithFee {
        refdata::Instrument instrument;
        refdata::FeeScheduleState fee;
    };

    explicit DeribitRefdataParser(std::shared_ptr<mapping::InstrumentMappingLoader> mapping);

    // POST /api/v2 with public/get_instruments — returns active
    // instruments whose canonical ID resolves via the mapping loader.
    std::vector<InstrumentWithFee> parse_instruments(const std::string& body, uint64_t collected_ts) const;

private:
    std::shared_ptr<mapping::InstrumentMappingLoader> mapping_;
};

}  // namespace bpt::refdata::adapter
