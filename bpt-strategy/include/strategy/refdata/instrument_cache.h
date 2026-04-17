#pragma once

#include "strategy/refdata/instrument.h"

#include <messages/DeltaUpdateType.h>
#include <messages/RefDataDelta.h>
#include <messages/RefDataSnapshot.h>

#include <optional>
#include <unordered_map>
#include <vector>

namespace bpt::strategy::refdata {

// Holds the local view of all instruments.
// Single-threaded: only call from the poll thread.
class InstrumentCache {
public:
    void apply_snapshot(bpt::messages::RefDataSnapshot& msg);

    // Apply a delta. Returns false if a sequence gap is detected (caller should
    // trigger a resubscription). Returns true on success or for heartbeats.
    bool apply_delta(bpt::messages::RefDataDelta& msg);

    void reset();

    [[nodiscard]] std::optional<Instrument> get(uint64_t instrument_id) const;
    [[nodiscard]] std::vector<Instrument> get_all() const;
    [[nodiscard]] std::size_t size() const { return cache_.size(); }
    [[nodiscard]] bool snapshot_received() const { return snapshot_received_; }
    [[nodiscard]] uint64_t last_delta_seq() const { return last_delta_seq_; }

private:
    std::unordered_map<uint64_t, Instrument> cache_;
    bool snapshot_received_{false};
    uint64_t snapshot_seq_num_{0};  // snapshotSeqNum from the last received snapshot
    uint64_t last_delta_seq_{0};    // last processed delta sequence number (0 = none yet)
};

}  // namespace bpt::strategy::refdata
