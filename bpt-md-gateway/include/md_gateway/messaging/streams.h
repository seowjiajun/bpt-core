#pragma once

/// \file
/// \brief Aeron stream IDs for the MdGateway ↔ Strategy IPC fabric.
///
/// Single source of truth for the four stream IDs the gateway uses.
/// Concrete publishers/subscribers in `messaging/` consume these via
/// the loaded `Settings.aeron` block; tests and ad-hoc consumers can
/// import the constants directly to stay in sync. FUNDING_RATE_STREAM_ID
/// migrated here from refdata when funding-rate publishing moved to the
/// gateway — the wire format is unchanged so strategy consumers were
/// not affected.

namespace bpt::md_gateway::messaging {

constexpr int MD_CONTROL_STREAM_ID = 2001;  ///< Strategy → MdGateway: MdSubscribeBatch
constexpr int MD_DATA_STREAM_ID = 2002;     ///< MdGateway → Strategy: MdMarketData / MdTrade / MdOrderBook
constexpr int MD_ACK_HB_STREAM_ID =
    2003;  ///< MdGateway → Strategy: MdSubscriptionAck / MdSubscriptionHeartbeat / MdServiceHeartbeat
constexpr int FUNDING_RATE_STREAM_ID =
    1005;  ///< MdGateway → Strategy: FundingRate (was refdata's; same ID + wire format)

}  // namespace bpt::md_gateway::messaging
