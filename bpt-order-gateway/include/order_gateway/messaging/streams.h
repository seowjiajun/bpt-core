#pragma once

namespace bpt::order_gateway::messaging {

constexpr int ORDER_STREAM_ID = 3001;             // Strategy -> Heimdall
constexpr int EXEC_REPORT_STREAM_ID = 3002;       // Heimdall -> Strategy
constexpr int HEARTBEAT_STREAM_ID = 3003;         // Heimdall -> Strategy
constexpr int ACCOUNT_SNAPSHOT_STREAM_ID = 3004;  // Heimdall -> Strategy

}  // namespace bpt::order_gateway::messaging
