#pragma once

/// \file
/// \brief Subscribes to bpt-md-gateway's InstrumentStats stream (typically 2004).
///
/// Slow-cadence per-instrument snapshot — OI, mark/index/last price, 24h volume.
/// Used by radar to enrich VolSurface points with OI for GEX / max-pain.

#include <Aeron.h>

#include <messages/InstrumentStats.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace bpt::radar::messaging {

class InstrumentStatsSubscriber {
public:
    using OnStatsFn = std::function<void(bpt::messages::InstrumentStats&)>;

    InstrumentStatsSubscriber(std::shared_ptr<aeron::Aeron> aeron, const std::string& channel, int stream_id);

    int poll(int fragment_limit = 8);

    OnStatsFn on_stats;

private:
    void handle_fragment(aeron::AtomicBuffer& buffer,
                         aeron::util::index_t offset,
                         aeron::util::index_t length,
                         aeron::Header& header);

    std::shared_ptr<aeron::Subscription> sub_;
};

}  // namespace bpt::radar::messaging
