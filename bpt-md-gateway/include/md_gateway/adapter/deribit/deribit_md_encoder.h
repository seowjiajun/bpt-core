#pragma once

/// \file
/// \brief Deribit MD JSON-RPC envelope builders.
///
/// Counterpart to DeribitMdDecoder (wire → internal). Deribit's MD WS
/// uses JSON-RPC 2.0; the envelope shape (jsonrpc/id/method/params) is
/// uniform — each builder fills in the method + params for its call.

#include <cstdint>
#include <string>

namespace bpt::md_gateway::adapter::deribit {

/// \brief public/subscribe on book + trades + ticker channels for one instrument.
///
/// \param depth  0 uses the lighter `quote.<sym>` channel; >0 uses
///               `book.<sym>.100ms` for full-depth ladder updates at
///               100 ms cadence.
[[nodiscard]] std::string build_subscribe_rpc(uint64_t rpc_id, const std::string& symbol, uint8_t depth);

/// \brief public/set_heartbeat — must be sent immediately after connect.
///
/// Deribit tears down the session within 30 s if test_request is not
/// answered with public/test (see build_test_response_rpc).
[[nodiscard]] std::string build_set_heartbeat_rpc(uint64_t rpc_id, int interval_s);

/// \brief public/test — reply to Deribit's heartbeat test_request.
[[nodiscard]] std::string build_test_response_rpc(uint64_t rpc_id);

}  // namespace bpt::md_gateway::adapter::deribit
