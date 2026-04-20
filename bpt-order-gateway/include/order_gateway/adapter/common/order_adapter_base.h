#pragma once

#include "order_gateway/adapter/common/i_order_adapter.h"
#include "order_gateway/config/settings.h"
#include "order_gateway/risk/disconnect_rate_breaker.h"
#include "order_gateway/util/exec_event_queue.h"

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <chrono>
#include <functional>
#include <thread>

namespace bpt::order_gateway::adapter {

// Common base for all exchange order adapters.
//
// Owns the IO thread, Boost.Asio io_context, TLS context, stop flag, and
// connected flag.  Subclasses implement connect_and_run() for one connection
// session; the base class drives the reconnect loop.
//
// Lifecycle:
//   start()  — spawns IO thread, calls run() → connect_and_run() in a loop
//   stop()   — sets stop_flag_, stops ioc_, joins thread
//
// connect_and_run() contract:
//   - Called with a freshly restarted ioc_.
//   - Should set connected_ = true once the session is ready.
//   - Return normally when stop_flag_ is set (clean shutdown).
//   - Throw std::exception on errors — base will log and reconnect.
//   - Base always sets connected_ = false after connect_and_run() returns or throws.
class OrderAdapterBase : public IOrderAdapter {
public:
    explicit OrderAdapterBase(const config::AdapterConfig& cfg);

    void start() override;
    void stop() override;

    // Drain pending exec events into fn.  Called from the main poll thread.
    // Returns the number of events drained.
    int drain_exec_events(const std::function<void(const ExecEvent&)>& fn) override;

    [[nodiscard]] bool is_halted() const override { return disconnect_breaker_.tripped(); }

    // Configure the disconnect-rate breaker. Call before start(). Safe
    // to leave at default (disabled) — tests and bootstrapped envs that
    // don't want the guard rail keep behaving as before.
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
    risk::DisconnectRateBreaker disconnect_breaker_{{}};  // disabled by default
    // Optional CPU-affinity topology. nullptr = fall back to legacy
    // cfg_.io_cpu TOML knob; both unset = unpinned.
    const bpt::common::util::Topology* topology_{nullptr};

    // Adapter IO thread pushes events here; main thread pops via drain_exec_events().
    // Capacity is config-driven (AdapterConfig.exec_queue_capacity) and must
    // be a power of 2 — enforced by the queue ctor.
    util::ExecEventQueue exec_queue_;

    // Implement to run one connection session.  Throw on error to trigger reconnect.
    virtual void connect_and_run() = 0;

    // Delay between reconnect attempts.  Default: 2s.
    virtual std::chrono::milliseconds reconnect_delay() const;

private:
    std::thread thread_;
    void run();
};

}  // namespace bpt::order_gateway::adapter
