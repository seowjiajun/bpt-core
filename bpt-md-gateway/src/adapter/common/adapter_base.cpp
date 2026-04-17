#include "md_gateway/adapter/common/adapter_base.h"

#include <thread>
#include <yggdrasil/util/thread_pin.h>

namespace bpt::md_gateway::adapter {

namespace ssl = boost::asio::ssl;

AdapterBase::AdapterBase(const config::AdapterConfig& cfg, std::shared_ptr<messaging::IMdPublisher> md_pub)
    : cfg_(cfg),
      md_pub_(std::move(md_pub)),
      validator_(cfg.max_price_deviation_pct),
      validating_pub_(*md_pub_, validator_),
      ssl_ctx_(ssl::context::tls_client) {
    ssl_ctx_.set_default_verify_paths();
    ssl_ctx_.set_verify_mode(ssl::verify_peer);
    // Enforce TLS 1.2 minimum — disable weak protocol versions.
    ssl_ctx_.set_options(ssl::context::no_tlsv1 | ssl::context::no_tlsv1_1);
}

void AdapterBase::subscribe(uint64_t instrument_id, std::string symbol, uint8_t depth) {
    subs_.subscribe(instrument_id, std::move(symbol), depth);
}

void AdapterBase::unsubscribe(uint64_t instrument_id) {
    subs_.unsubscribe(instrument_id);
}

void AdapterBase::start() {
    pub_thread_ = std::thread([this]() { publish_loop(); });
    thread_ = std::thread([this]() { run(); });
}

void AdapterBase::stop() {
    stop_flag_.store(true, std::memory_order_relaxed);
    ioc_.stop();
    if (thread_.joinable())
        thread_.join();
    if (pub_thread_.joinable())
        pub_thread_.join();
}

std::chrono::milliseconds AdapterBase::reconnect_delay() const {
    return std::chrono::seconds(1);
}

void AdapterBase::push_frame(std::string_view payload, uint64_t recv_ns) noexcept {
    if (!frame_queue_.try_push(recv_ns, payload)) {
        ++dropped_frames_;
        // Log at most once every 1000 drops to avoid flooding on sustained backpressure.
        if (dropped_frames_ == 1 || dropped_frames_ % 1000 == 0) {
            ygg::log::warn("{}: frame queue full or oversized — dropped frames: {}", exchange_name(), dropped_frames_);
        }
    }
}

void AdapterBase::publish_loop() {
    while (!stop_flag_.load(std::memory_order_relaxed)) {
        bool processed =
            frame_queue_.try_pop([this](uint64_t recv_ns, std::string_view payload) { parse_frame(payload, recv_ns); });
        if (!processed)
            std::this_thread::yield();
    }
    // Drain any frames queued between the IO thread stopping and publish_loop waking.
    while (
        frame_queue_.try_pop([this](uint64_t recv_ns, std::string_view payload) { parse_frame(payload, recv_ns); })) {
    }
}

void AdapterBase::run() {
    ygg::util::pin_thread_to_cpu(cfg_.io_thread_cpu, exchange_name());
    while (!stop_flag_.load(std::memory_order_relaxed)) {
        try {
            ioc_.restart();
            validator_.reset();
            auto ws = connect_and_subscribe();
            if (!ws) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            if (on_connect)
                on_connect();
            read_loop(*ws);  // NOLINT(bugprone-unchecked-optional-access)
        } catch (const std::exception& e) {
            if (!stop_flag_.load(std::memory_order_relaxed)) {
                if (on_disconnect)
                    on_disconnect();
                ygg::log::error("{} error: {}, reconnecting in {}ms",
                                exchange_name(),
                                e.what(),
                                reconnect_delay().count());
                std::this_thread::sleep_for(reconnect_delay());
            }
        }
    }
}

}  // namespace bpt::md_gateway::adapter
