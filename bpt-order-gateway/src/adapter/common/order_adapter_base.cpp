#include "order_gateway/adapter/common/order_adapter_base.h"

#include <bpt_common/logging.h>
#include <bpt_common/util/thread_pin.h>
#include <bpt_common/util/tsc_clock.h>

namespace bpt::order_gateway::adapter {

namespace ssl = boost::asio::ssl;

OrderAdapterBase::OrderAdapterBase(const config::AdapterConfig& cfg)
    : cfg_(cfg),
      ssl_ctx_(ssl::context::tls_client),
      exec_queue_(cfg.exec_queue_capacity) {
    ssl_ctx_.set_default_verify_paths();
    ssl_ctx_.set_verify_mode(ssl::verify_peer);
}

void OrderAdapterBase::start() {
    thread_ = std::thread([this] { run(); });
}

void OrderAdapterBase::stop() {
    stop_flag_.store(true, std::memory_order_relaxed);
    ioc_.stop();
    if (thread_.joinable())
        thread_.join();
}

int OrderAdapterBase::drain_exec_events(const std::function<void(const ExecEvent&)>& fn) {
    int n = 0;
    while (exec_queue_.try_pop([&](const ExecEvent& ev) { fn(ev); }))
        ++n;
    return n;
}

std::chrono::milliseconds OrderAdapterBase::reconnect_delay() const {
    return std::chrono::seconds(2);
}

void OrderAdapterBase::run() {
    bpt::common::util::pin_thread_to_cpu(cfg_.io_cpu, exchange_name());

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        try {
            ioc_.restart();
            connect_and_run();
        } catch (const std::exception& e) {
            if (!stop_flag_.load(std::memory_order_relaxed)) {
                bpt::common::log::error("{} error: {}, reconnecting in {}ms",
                                exchange_name(),
                                e.what(),
                                reconnect_delay().count());
                // Disconnect-rate breaker: treat every caught exception as
                // one disconnect event. We only count on caught errors
                // (not on clean shutdown), so the breaker won't trip at
                // service stop. Latch check runs before the sleep so the
                // loud ERROR log lands promptly after the trip.
                const bool was_tripped = disconnect_breaker_.tripped();
                disconnect_breaker_.record(bpt::common::util::TscClock::now_epoch_ns());
                if (!was_tripped && disconnect_breaker_.tripped()) {
                    bpt::common::log::error(
                        "{} DISCONNECT BREAKER TRIPPED — {} reconnects "
                        "in last {}s (threshold {}). Halting new orders to this venue. "
                        "Restart service after human review to resume.",
                        exchange_name(),
                        disconnect_breaker_.count_in_window(),
                        disconnect_breaker_.config().window_ns / 1'000'000'000ULL,
                        disconnect_breaker_.config().threshold);
                }
                std::this_thread::sleep_for(reconnect_delay());
            }
        }
        connected_.store(false, std::memory_order_relaxed);
    }
}

}  // namespace bpt::order_gateway::adapter
