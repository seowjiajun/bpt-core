#pragma once

namespace bpt::md_gateway::messaging {

// Aeron stream IDs for Huginn ↔ Strategy communication.
// Stream 2001: Strategy → Huginn  (MdSubscribeBatch)
// Stream 2002: Huginn → Strategy  (MdMarketData, MdTrade)
// Stream 2003: Huginn → Strategy  (MdSubscriptionAck, MdSubscriptionHeartbeat, MdServiceHeartbeat)
// Stream 1005: Huginn → Strategy  (FundingRate) — moved from Refdata; same stream ID, same wire format
constexpr int MD_CONTROL_STREAM_ID = 2001;
constexpr int MD_DATA_STREAM_ID = 2002;
constexpr int MD_ACK_HB_STREAM_ID = 2003;
constexpr int FUNDING_RATE_STREAM_ID = 1005;

}  // namespace bpt::md_gateway::messaging
