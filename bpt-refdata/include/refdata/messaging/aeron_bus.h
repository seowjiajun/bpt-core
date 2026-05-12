#pragma once

/// \file
/// \brief Composition root for the Aeron-backed bus adapters.
///
/// Bundles construction of all five Aeron sinks/sources so main.cpp wires
/// "the bus" as one unit rather than five inline `make_unique` calls. Adding
/// a new port = one field + one line in build(); main.cpp doesn't change.

#include "refdata/port/i_fee_schedule_sink.h"
#include "refdata/port/i_refdata_control_source.h"
#include "refdata/port/i_refdata_delta_sink.h"
#include "refdata/port/i_refdata_snapshot_sink.h"
#include "refdata/port/i_refdata_status_sink.h"

#include <Aeron.h>

#include <memory>

namespace bpt::refdata::config {
struct Settings;
}

namespace bpt::refdata::messaging {

struct AeronBus {
    std::unique_ptr<port::IRefdataControlSource> control_source;
    std::unique_ptr<port::IRefdataSnapshotSink> snapshot_sink;
    std::shared_ptr<port::IRefdataDeltaSink> delta_sink;
    std::shared_ptr<port::IFeeScheduleSink> fee_sink;
    std::shared_ptr<port::IRefdataStatusSink> status_sink;

    /// \brief Build all five Aeron-backed adapters wired to the channels
    ///        and stream IDs in `settings`.
    static AeronBus build(std::shared_ptr<aeron::Aeron> aeron, const config::Settings& settings);
};

}  // namespace bpt::refdata::messaging
