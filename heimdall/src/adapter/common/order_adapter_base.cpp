#include "heimdall/adapter/common/order_adapter_base.h"

#include <yggdrasil/util/thread_pin.h>

namespace heimdall::adapter {

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
    ygg::util::pin_thread_to_cpu(cfg_.io_cpu, exchange_name());

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        try {
            ioc_.restart();
            connect_and_run();
        } catch (const std::exception& e) {
            if (!stop_flag_.load(std::memory_order_relaxed)) {
                ygg::log::error("[Heimdall] {} error: {}, reconnecting in {}ms",
                                exchange_name(),
                                e.what(),
                                reconnect_delay().count());
                std::this_thread::sleep_for(reconnect_delay());
            }
        }
        connected_.store(false, std::memory_order_relaxed);
    }
}

}  // namespace heimdall::adapter
