#pragma once

#include "strategy/strategy/i_strategy.h"

#include <Aeron.h>

#include <Publication.h>
#include <bpt_common/aeron/stream_config.h>
#include <cstdint>
#include <memory>
#include <string>

namespace bpt::strategy::console {

// Publishes the strategy's PortfolioState to the console bridge as a JSON
// blob over Aeron. The bridge relays the payload as-is to all connected
// console clients.
//
// Throttled to ~10 Hz internally; callers can poke `publish_if_due()` from a
// hot poll loop without worrying about cadence. Snapshots that contain no
// option legs and no surface points are skipped to avoid spamming the
// console with empty payloads from linear strategies.
class PortfolioSnapshotPublisher {
public:
    // stream.channel/stream_id may be empty/0 — in that case the publisher
    // operates as a no-op so call sites don't need conditionals. Used when
    // running in backtest mode or with no console configured.
    PortfolioSnapshotPublisher(std::shared_ptr<aeron::Aeron> aeron, const bpt::common::config::StreamConfig& stream);

    // Publishes a snapshot if at least kIntervalNs has elapsed since the
    // previous publish AND the snapshot has content. now_ns must come from
    // the same clock the caller uses for throttling decisions (TscClock).
    void publish_if_due(const strategy::PortfolioState& state, uint64_t now_ns);

    // Offer a raw buffer on the same publication. Used by StrategyService to
    // publish strategy state JSON alongside portfolio snapshots.
    void offer_raw(aeron::AtomicBuffer& buf, int32_t length) {
        if (pub_)
            pub_->offer(buf, 0, length);
    }

    bool is_active() const noexcept { return static_cast<bool>(pub_); }

private:
    static constexpr uint64_t kIntervalNs = 100'000'000ULL;  // 10 Hz

    void publish(const strategy::PortfolioState& state, uint64_t now_ns);

    std::shared_ptr<aeron::Publication> pub_;
    uint64_t last_publish_ns_{0};
};

}  // namespace bpt::strategy::console
