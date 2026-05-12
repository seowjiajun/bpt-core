#pragma once

/// \file
/// \brief Deribit MD WebSocket client — RunLoop subclass owned by DeribitMdAdapter.

#include "md_gateway/adapter/common/subscription_map.h"
#include "md_gateway/config/settings.h"

#include <atomic>
#include <bpt_common/ws/run_loop.h>
#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>

namespace bpt::md_gateway::adapter {

/// \brief Drives the Deribit MD WS read/send/ping loop.
///
/// Owns the venue WS protocol bits:
///   - on_frame forwards real application frames to the adapter, and
///     services any pending test_request response set via
///     `signal_test_request()` from the publisher thread.
///   - on_tick services test_request and drains pending subscriptions
///     when the WS goes quiet.
///   - No ping_config — Deribit's `set_heartbeat` keeps the session
///     alive at the application layer.
///
/// rpc_id_ is owned here so the wire-protocol concern (sequencing JSON-RPC
/// IDs) lives entirely on the WS client.
class DeribitMdWsClient : public bpt::common::ws::RunLoop {
public:
    using FrameHandler = std::function<void(std::string_view payload, uint64_t recv_ns)>;

    DeribitMdWsClient(const config::AdapterConfig& cfg, SubscriptionMap& subs) : cfg_(cfg), subs_(subs) {}

    void set_frame_handler(FrameHandler h) { handler_ = std::move(h); }

    /// \brief Allocate a fresh JSON-RPC id (monotonic).
    ///
    /// Exposed so the adapter can use it for the heartbeat / initial
    /// subscribe writes done before run() takes ownership of the stream.
    uint64_t next_rpc_id() noexcept { return rpc_id_.fetch_add(1, std::memory_order_relaxed); }

    /// \brief Mark that the next on_frame / on_tick should send a test response.
    ///
    /// Called from the publisher thread (decoder reports a test_request
    /// heartbeat) → IO thread (sends `public/test`). atomic flag, no
    /// locking required.
    void signal_test_request() noexcept { needs_test_response_.store(true, std::memory_order_release); }

protected:
    void on_frame(std::string_view payload, uint64_t recv_ns) override;
    void on_tick() override;

private:
    void send_test_response_if_pending() noexcept;

    const config::AdapterConfig& cfg_;
    SubscriptionMap& subs_;
    FrameHandler handler_;
    std::atomic<uint64_t> rpc_id_{1};
    std::atomic<bool> needs_test_response_{false};
};

}  // namespace bpt::md_gateway::adapter
