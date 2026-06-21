#include "order_gateway/adapter/common/order_adapter_base.h"

#include <bpt_common/logging.h>
#include <bpt_common/util/strings.h>
#include <bpt_common/util/thread_name.h>
#include <bpt_common/util/thread_pin.h>
#include <bpt_common/util/tsc_clock.h>
#include <string>
#include <utility>
#include <variant>

namespace bpt::order_gateway::adapter {

namespace ssl = boost::asio::ssl;

namespace {

std::string io_role(const char* exchange) {
    return "ogw." + bpt::common::util::to_lower(exchange) + ".io";
}

std::string io_thread_name(const char* exchange) {
    return "ogw-" + bpt::common::util::to_lower(exchange) + "-io";
}

std::string send_thread_name(const char* exchange) {
    return "ogw-" + bpt::common::util::to_lower(exchange) + "-sx";
}

}  // namespace

OrderAdapterBase::OrderAdapterBase(const config::AdapterConfig& cfg)
    : cfg_(cfg),
      ssl_ctx_(ssl::context::tls_client),
      exec_queue_(cfg.exec_queue_capacity),
      rest_exec_queue_(cfg.exec_queue_capacity) {
    ssl_ctx_.set_default_verify_paths();
    ssl_ctx_.set_verify_mode(ssl::verify_peer);
}

void OrderAdapterBase::start() {
    thread_ = std::thread([this] { run(); });
    send_executor_thread_ = std::thread([this] { run_send_executor(); });
}

void OrderAdapterBase::stop() {
    stop_flag_.store(true, std::memory_order_relaxed);
    send_work_queue_.close();
    ioc_.stop();
    if (thread_.joinable())
        thread_.join();
    if (send_executor_thread_.joinable())
        send_executor_thread_.join();
}

void OrderAdapterBase::send_new_order(const order::NewOrderEvent& order) {
    util::NewOrderRequest req;
    req.order_id = order.order_id;
    req.instrument_id = order.instrument_id;
    req.price = order.price;
    req.quantity = order.quantity;
    req.side = order.side;
    req.order_type = order.order_type;
    req.tif = order.tif;
    req.exec_inst = order.exec_inst;
    req.exchange_symbol = order.exchange_symbol;
    send_work_queue_.push(util::SendWorkItem{std::move(req)});
}

void OrderAdapterBase::send_cancel(const order::CancelOrderEvent& cancel, const std::string& native_symbol) {
    util::CancelRequest req;
    req.order_id = cancel.order_id;
    req.native_symbol = native_symbol;
    send_work_queue_.push(util::SendWorkItem{std::move(req)});
}

void OrderAdapterBase::send_cancel_all(uint64_t instrument_id) {
    util::CancelAllRequest req;
    req.instrument_id = instrument_id;
    send_work_queue_.push(util::SendWorkItem{std::move(req)});
}

void OrderAdapterBase::send_modify(const order::ModifyOrderEvent& modify, const std::string& native_symbol) {
    util::ModifyRequest req;
    req.order_id = modify.order_id;
    req.new_price = modify.new_price;
    req.new_quantity = modify.new_quantity;
    req.native_symbol = native_symbol;
    send_work_queue_.push(util::SendWorkItem{std::move(req)});
}

int OrderAdapterBase::drain_exec_events(const std::function<void(const ExecEvent&)>& fn) {
    int n = 0;
    while (exec_queue_.try_pop([&](const ExecEvent& ev) { fn(ev); }))
        ++n;
    while (rest_exec_queue_.try_pop([&](const ExecEvent& ev) { fn(ev); }))
        ++n;
    return n;
}

std::chrono::milliseconds OrderAdapterBase::reconnect_delay() const {
    return std::chrono::seconds(2);
}

void OrderAdapterBase::run() {
    bpt::common::util::set_thread_name(io_thread_name(exchange_name()));
    bool pinned_via_topology = false;
    if (topology_)
        pinned_via_topology =
            bpt::common::util::pin_thread_by_role(*topology_, io_role(exchange_name()), exchange_name());
    if (!pinned_via_topology)
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

void OrderAdapterBase::run_send_executor() {
    bpt::common::util::set_thread_name(send_thread_name(exchange_name()));
    while (true) {
        auto item_opt = send_work_queue_.pop_blocking();
        if (!item_opt)
            return;
        try {
            std::visit(
                [this](auto&& req) {
                    using T = std::decay_t<decltype(req)>;
                    if constexpr (std::is_same_v<T, util::NewOrderRequest>) {
                        do_send_new_order_blocking(req);
                    } else if constexpr (std::is_same_v<T, util::CancelRequest>) {
                        do_send_cancel_blocking(req);
                    } else if constexpr (std::is_same_v<T, util::CancelAllRequest>) {
                        do_send_cancel_all_blocking(req);
                    } else if constexpr (std::is_same_v<T, util::ModifyRequest>) {
                        do_send_modify_blocking(req);
                    }
                },
                *item_opt);
        } catch (const std::exception& e) {
            bpt::common::log::error("{}: send executor caught exception: {}", exchange_name(), e.what());
        }
    }
}

}  // namespace bpt::order_gateway::adapter
