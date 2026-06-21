#pragma once

/// \file
/// \brief Common base for all order-gateway adapters — owns IO thread,
///        send-executor thread, and the two ExecEvent queues.
///
/// Greenfield (LMAX-style) shape:
///
///   [main poll thread]
///        │  send_new_order/cancel/modify           drain_exec_events()
///        ▼                                                ▲
///   send_work_queue_ ──► [send executor thread] ─┐   ┌────┴────┐
///   (mutex+deque)         do_send_*_blocking      │   │         │
///                         (HTTPS POST, WS post,   │   │         │
///                          future.wait_for, ...)  ▼   ▼         │
///                                            rest_exec_queue_  exec_queue_
///                                            (SPSC: exec→main) (SPSC: WS→main)
///                                                              ▲
///                              [adapter IO thread]              │
///                              ws read + decode ────────────────┘
///
/// Single-writer per queue:
///   - exec_queue_      ← only the WS IO thread pushes.
///   - rest_exec_queue_ ← only the send-executor thread pushes.
///   - main thread is the sole consumer of both, drained on every loop.
///
/// Per-adapter venue logic implements the `do_send_*_blocking` hooks
/// (called on the send-executor thread). The base provides non-blocking
/// `send_*` that simply value-copies the SBE message into a
/// `util::SendWorkItem` and pushes onto `send_work_queue_`.

#include "order_gateway/adapter/common/i_order_adapter.h"
#include "order_gateway/config/settings.h"
#include "order_gateway/risk/disconnect_rate_breaker.h"
#include "order_gateway/util/exec_event_queue.h"
#include "order_gateway/util/send_work_item.h"
#include "order_gateway/util/send_work_queue.h"

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <chrono>
#include <functional>
#include <thread>

namespace bpt::order_gateway::adapter {

/// \brief Common base for all exchange order adapters.
///
/// Owns the IO thread, send-executor thread, Boost.Asio io_context, TLS
/// context, stop flag, connected flag, and both ExecEvent queues.
///
/// **Lifecycle**:
///   - `start()` — spawns IO thread + send-executor thread.
///   - `stop()`  — closes send queue, sets stop_flag_, stops ioc_, joins
///                  both threads.
///
/// **`connect_and_run()` contract** (subclass-implemented):
///   - Called with a freshly restarted ioc_.
///   - Should set connected_ = true once the session is ready.
///   - Return normally when stop_flag_ is set (clean shutdown).
///   - Throw std::exception on errors — base will log and reconnect.
class OrderAdapterBase : public IOrderAdapter {
public:
    explicit OrderAdapterBase(const config::AdapterConfig& cfg);

    void start() override;
    void stop() override;

    /// \name Non-blocking order entry
    /// Push a `SendWorkItem` onto `send_work_queue_` and return.
    /// The send-executor thread drains and invokes `do_send_*_blocking`.
    /// @{
    void send_new_order(const order::NewOrderEvent& order) final;
    void send_cancel(const order::CancelOrderEvent& cancel, const std::string& native_symbol) final;
    void send_cancel_all(uint64_t instrument_id) final;
    void send_modify(const order::ModifyOrderEvent& modify, const std::string& native_symbol) final;
    /// @}

    /// \brief Drain pending exec events. Called from the main poll thread.
    ///
    /// Drains BOTH queues (WS-fed `exec_queue_` and executor-fed
    /// `rest_exec_queue_`). Ordering between the two queues is
    /// best-effort — WS-fed events are drained first because they carry
    /// the higher-frequency fill stream.
    /// \return Total number of events drained.
    int drain_exec_events(const std::function<void(const ExecEvent&)>& fn) override;

    [[nodiscard]] bool is_halted() const override { return disconnect_breaker_.tripped(); }

    void set_disconnect_breaker_config(risk::DisconnectRateBreaker::Config cfg) override {
        disconnect_breaker_.reset(cfg);
    }

    void set_topology(const bpt::common::util::Topology& topology) override { topology_ = &topology; }

protected:
    config::AdapterConfig cfg_;
    boost::asio::io_context ioc_;
    boost::asio::ssl::context ssl_ctx_;
    std::atomic<bool> stop_flag_{false};
    std::atomic<bool> connected_{false};
    risk::DisconnectRateBreaker disconnect_breaker_{{}};
    const bpt::common::util::Topology* topology_{nullptr};

    /// WS IO thread → main thread. Single-producer (the adapter's
    /// WebSocket read loop). Capacity from cfg_.exec_queue_capacity.
    util::ExecEventQueue exec_queue_;

    /// Send-executor thread (primary) → main thread. MPSC because some
    /// adapters have a secondary producer on rare paths (e.g. HL's
    /// reconciler worker thread emits recovered ExecEvents). Lockless
    /// pop; mutex on push. Carries ACKs from REST/post-action responses,
    /// synthetic rejects, and any other event sourced from the blocking
    /// send path.
    util::MpscExecEventQueue rest_exec_queue_;

    /// Main thread → send-executor thread. The main loop pushes a
    /// `SendWorkItem` per `send_*` call; the executor consumes one at a
    /// time and dispatches to `do_send_*_blocking`.
    util::SendWorkQueue send_work_queue_;

    /// \brief Run one connection session. Throw on error to trigger reconnect.
    virtual void connect_and_run() = 0;

    /// \brief Delay between reconnect attempts. Default: 2s.
    virtual std::chrono::milliseconds reconnect_delay() const;

    /// \name Venue-specific blocking send hooks
    /// Invoked on the send-executor thread. Must:
    ///   - serialise / sign / encode the venue payload,
    ///   - send (HTTPS POST, WS write, WS post_action+wait, etc.),
    ///   - push the resulting ExecEvent(s) onto `rest_exec_queue_` (NOT
    ///     `exec_queue_`, which is reserved for the WS IO thread).
    /// @{
    virtual void do_send_new_order_blocking(const util::NewOrderRequest& req) = 0;
    virtual void do_send_cancel_blocking(const util::CancelRequest& req) = 0;
    virtual void do_send_cancel_all_blocking(const util::CancelAllRequest& req) = 0;
    virtual void do_send_modify_blocking(const util::ModifyRequest& req) = 0;
    /// @}

private:
    std::thread thread_;
    std::thread send_executor_thread_;
    void run();
    void run_send_executor();
};

}  // namespace bpt::order_gateway::adapter
