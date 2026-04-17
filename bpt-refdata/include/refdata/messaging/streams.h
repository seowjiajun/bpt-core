#pragma once

namespace bpt::refdata::messaging {

// Instrument refdata (Refdata ↔ Strategy)
constexpr int REFDATA_SNAPSHOT_STREAM_ID = 1001;  // Refdata → Strategy (RefDataSnapshot)
constexpr int REFDATA_DELTA_STREAM_ID = 1002;     // Refdata → Strategy (RefDataDelta, heartbeat)
constexpr int REFDATA_CONTROL_STREAM_ID = 1003;   // Strategy → Refdata (RefDataSubscriptionRequest)

// Exchange-sourced refdata (Refdata → Strategy)
constexpr int FEE_SCHEDULE_STREAM_ID = 1004;  // FeeSchedule messages
// Note: stream 1005 (FundingRate) has moved to Huginn — same stream ID, same wire format
constexpr int MUNINN_STATUS_STREAM_ID = 1006;  // RefDataReady + RefDataError messages

}  // namespace bpt::refdata::messaging
