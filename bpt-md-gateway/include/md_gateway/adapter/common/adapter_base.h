#pragma once

/// \file
/// \brief Common base class for bpt-md-gateway venue adapters.
///
/// AdapterBase owns the IO + publisher threads and the WS reconnect loop.
/// Each venue subclass plugs in connect/read/parse via the protected
/// virtual hooks. md-recorder reuses the same adapter library by
/// substituting recording-aware on_frame() overrides on derived classes.
///
/// Templated on Pub (the concrete inner publisher type) so the
/// hot path decoder → ValidatingPublisher<Pub> → Pub is vtable-free.
/// Prod md-gateway instantiates AdapterBase<MdPublisher>; md-recorder
/// instantiates AdapterBase<NoopMdPublisher>; tests can use any concrete
/// type satisfying the publisher signature.

#include "md_gateway/adapter/common/i_adapter.h"
#include "md_gateway/adapter/common/subscription_map.h"
#include "md_gateway/config/settings.h"
#include "md_gateway/md/md_validator.h"
#include "md_gateway/md/validating_publisher.h"

#include <algorithm>
#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <cctype>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <bpt_common/logging.h>
#include <bpt_common/util/spsc_queue.h>
#include <bpt_common/util/thread_name.h>
#include <bpt_common/util/thread_pin.h>
#include <bpt_common/ws/ws_connect.h>

namespace bpt::md_gateway::adapter {

namespace detail {

inline std::string lowercase_venue(const char* exchange) {
    std::string venue = exchange;
    std::transform(venue.begin(), venue.end(), venue.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return venue;
}

// Compose the topology role name used by md-gateway IO threads:
// "mdgw.<venue-lower>.io". Keeps the role vocabulary in sync with
// the service_name abbreviation used elsewhere (bpt-mdgw-<venue>).
inline std::string io_role(const char* exchange) {
    return "mdgw." + lowercase_venue(exchange) + ".io";
}

// OS thread names for the two AdapterBase threads. Venue in the middle
// so sort order groups all threads of the same venue together in ps -L
// (mdgw-okx-io, mdgw-okx-log, mdgw-okx-pub sit adjacent alphabetically).
// Matches the existing quill-backend (mdgw-<venue>-log) and topology-role
// (mdgw.<venue>.<subsystem>) ordering. 15-char cap per Linux TASK_COMM_LEN.
inline std::string io_thread_name(const char* exchange) {
    return "mdgw-" + lowercase_venue(exchange) + "-io";
}
inline std::string pub_thread_name(const char* exchange) {
    return "mdgw-" + lowercase_venue(exchange) + "-pub";
}

}  // namespace detail

/// \brief Base class for every exchange market-data adapter.
///
/// Owns the common lifecycle state — io_context, ssl_context, subscription
/// map, stop flag — and two threads:
///   - **IO thread** drives WebSocket receive, stamps recv_ns, pushes raw
///     frames into frame_queue_.
///   - **Publisher thread** drains frame_queue_, calls parse_frame() → Aeron.
///
/// Subclasses implement:
///   - connect_and_subscribe() — open WS, send initial subscribe frames.
///   - read_loop(ws) — receive loop; call push_frame() for each data frame.
///   - parse_frame(payload, recv_ns) — venue parser + md_pub_ publish.
///
/// The reconnect loop in run() calls connect_and_subscribe() + read_loop()
/// in a tight try/catch. Subclasses may override reconnect_delay()
/// (default 1 s).
template <class Pub>
class AdapterBase : public IAdapter {
public:
    /// 512 slots × 16 KiB ≈ 8 MiB per adapter. 16 KiB covers the largest
    /// expected WS frame (Deribit book snapshot at depth=255 is ~15 KiB).
    /// Bump SLOT_BYTES if a venue starts emitting bigger frames.
    static constexpr size_t QUEUE_CAPACITY = 512;
    static constexpr size_t SLOT_BYTES = 16384;
    using FrameQueue = bpt::common::util::SpscQueue<QUEUE_CAPACITY, SLOT_BYTES>;

    AdapterBase(const config::AdapterConfig& cfg, std::shared_ptr<Pub> md_pub)
        : cfg_(cfg),
          md_pub_(std::move(md_pub)),
          validator_(cfg.max_price_deviation_pct),
          validating_pub_(*md_pub_, validator_, cfg_.exchange.c_str()),
          ssl_ctx_(boost::asio::ssl::context::tls_client) {
        ssl_ctx_.set_default_verify_paths();
        ssl_ctx_.set_verify_mode(boost::asio::ssl::verify_peer);
        // Enforce TLS 1.2 minimum — disable weak protocol versions.
        ssl_ctx_.set_options(boost::asio::ssl::context::no_tlsv1 |
                             boost::asio::ssl::context::no_tlsv1_1);

        // Apply validation-drop breaker config from the adapter's TOML block.
        // Default-constructed Config is disabled, so adapters that don't set
        // the knobs behave identically to before this change.
        md::ValidationDropBreaker::Config db_cfg;
        db_cfg.enabled = cfg_.validation_drop_breaker_enabled;
        db_cfg.threshold_pct = cfg_.validation_drop_threshold_pct;
        db_cfg.window_ns = static_cast<uint64_t>(cfg_.validation_drop_window_sec) * 1'000'000'000ULL;
        db_cfg.min_events = cfg_.validation_drop_min_events;
        validating_pub_.set_drop_breaker_config(db_cfg);
    }

    ~AdapterBase() override = default;

    void subscribe(uint64_t instrument_id, std::string symbol, uint8_t depth = 0) override {
        subs_.subscribe(instrument_id, std::move(symbol), depth);
    }

    void unsubscribe(uint64_t instrument_id) override {
        subs_.unsubscribe(instrument_id);
    }

    void start() override {
        pub_thread_ = std::thread([this]() { publish_loop(); });
        thread_ = std::thread([this]() { run(); });
    }

    void stop() override {
        stop_flag_.store(true, std::memory_order_relaxed);
        ioc_.stop();
        if (thread_.joinable())
            thread_.join();
        if (pub_thread_.joinable())
            pub_thread_.join();
    }

    void set_topology(const bpt::common::util::Topology& topology) override { topology_ = &topology; }

    [[nodiscard]] uint64_t md_published_count() const noexcept override { return validating_pub_.published(); }
    [[nodiscard]] uint64_t validation_drop_count() const noexcept override { return validating_pub_.drops(); }
    [[nodiscard]] bool validation_drop_breaker_tripped() const noexcept override {
        return validating_pub_.breaker_tripped();
    }

protected:
    /// \brief Backoff before the next reconnect attempt. Default 1 s.
    virtual std::chrono::milliseconds reconnect_delay() const {
        return std::chrono::seconds(1);
    }

    /// \brief Open the WebSocket and send all initial subscribe frames.
    /// \return nullptr if no subscriptions exist yet — run() retries in 100 ms.
    virtual std::unique_ptr<bpt::common::ws::AnyWsStream> connect_and_subscribe() = 0;

    /// \brief Synchronous receive loop.
    ///
    /// Implementations call push_frame() for each data frame and throw on
    /// any fatal error to trigger a reconnect.
    virtual void read_loop(bpt::common::ws::AnyWsStream& ws) = 0;

    /// \brief Publisher-thread callback invoked once per dequeued frame.
    ///
    /// Implementations run the venue parser then call md_pub_ publish methods.
    virtual void parse_frame(std::string_view payload, uint64_t recv_ns) = 0;

    /// \brief IO-thread seam invoked by the venue ws-client for each application frame.
    ///
    /// Default implementation enqueues onto the SPSC frame queue for the
    /// publisher thread (push_frame). md-recorder overrides this to tee
    /// the raw bytes into a RawSpool before enqueueing — keeps the
    /// recording tap out of the main mdgw source.
    virtual void handle_frame(std::string_view payload, uint64_t recv_ns) noexcept {
        push_frame(payload, recv_ns);
    }

    /// \brief Push a raw WS frame onto the SPSC queue. IO-thread only.
    ///
    /// Logs a throttled warning when the queue is full or the frame is
    /// oversized — never blocks the receive path.
    void push_frame(std::string_view payload, uint64_t recv_ns) noexcept {
        if (!frame_queue_.try_push(recv_ns, payload)) {
            ++dropped_frames_;
            // Log at most once every 1000 drops to avoid flooding on sustained backpressure.
            if (dropped_frames_ == 1 || dropped_frames_ % 1000 == 0) {
                bpt::common::log::warn("{}: frame queue full or oversized — dropped frames: {}",
                                        this->exchange_name(), dropped_frames_);
            }
        }
    }

    config::AdapterConfig cfg_;
    std::shared_ptr<Pub> md_pub_;
    /// Optional CPU-affinity topology. Pointer (not reference) because the
    /// base can be constructed before topology is known; set via
    /// set_topology() before start(). nullptr = fall back to the legacy
    /// cfg_.io_thread_cpu TOML knob.
    const bpt::common::util::Topology* topology_{nullptr};
    /// validator_ must be declared before validating_pub_ (init-list order).
    md::MdValidator validator_;
    md::ValidatingPublisher<Pub> validating_pub_;

    SubscriptionMap subs_;
    boost::asio::io_context ioc_;
    boost::asio::ssl::context ssl_ctx_;

    /// Cache-line isolated. publish_loop checks stop_flag_ on every
    /// spin iteration; without this, it could share a line with the
    /// frame_queue_'s producer-side `head_` atomic and cause every
    /// IO-thread push to invalidate the consumer's stop_flag check.
    alignas(64) std::atomic<bool> stop_flag_{false};

    FrameQueue frame_queue_;

private:
    void run() {
        bpt::common::util::set_thread_name(detail::io_thread_name(this->exchange_name()));
        // Pin policy: prefer central Topology role assignment when set;
        // fall back to the legacy per-adapter cfg_.io_thread_cpu knob for
        // configs that haven't migrated. Both unset = unpinned.
        bool pinned_via_topology = false;
        if (topology_)
            pinned_via_topology = bpt::common::util::pin_thread_by_role(
                *topology_, detail::io_role(this->exchange_name()), this->exchange_name());
        if (!pinned_via_topology)
            bpt::common::util::pin_thread_to_cpu(cfg_.io_thread_cpu, this->exchange_name());
        while (!stop_flag_.load(std::memory_order_relaxed)) {
            try {
                ioc_.restart();
                validator_.reset();
                auto ws = connect_and_subscribe();
                if (!ws) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
                if (this->on_connect)
                    this->on_connect();
                read_loop(*ws);
            } catch (const std::exception& e) {
                if (!stop_flag_.load(std::memory_order_relaxed)) {
                    if (this->on_disconnect)
                        this->on_disconnect();
                    bpt::common::log::error("{} error: {}, reconnecting in {}ms",
                                    this->exchange_name(),
                                    e.what(),
                                    reconnect_delay().count());
                    std::this_thread::sleep_for(reconnect_delay());
                }
            }
        }
    }

    void publish_loop() {
        bpt::common::util::set_thread_name(detail::pub_thread_name(this->exchange_name()));

        // Adaptive backoff: tight spin with `pause` while the queue is hot,
        // then yield when it stays empty long enough that we're paying real
        // CPU for nothing. Picking the spin budget conservatively (~few µs
        // worth of pause iterations on x86) keeps wake-up latency in the
        // tens of nanoseconds when the IO thread pushes between consumer
        // iterations — std::this_thread::yield() on a pinned/isolated core
        // is a wasted syscall that adds ~µs of context-switch jitter.
        constexpr int kSpinBudget = 1000;
        int empty_iters = 0;
        while (!stop_flag_.load(std::memory_order_relaxed)) {
            const bool processed =
                frame_queue_.try_pop([this](uint64_t recv_ns, std::string_view payload) { parse_frame(payload, recv_ns); });
            if (processed) {
                empty_iters = 0;
                continue;
            }
            if (++empty_iters < kSpinBudget) {
#if defined(__x86_64__) || defined(__i386__)
                __builtin_ia32_pause();
#elif defined(__aarch64__)
                asm volatile("yield" ::: "memory");
#endif
            } else {
                std::this_thread::yield();
                empty_iters = kSpinBudget;
            }
        }
        // Drain any frames queued between the IO thread stopping and publish_loop waking.
        while (
            frame_queue_.try_pop([this](uint64_t recv_ns, std::string_view payload) { parse_frame(payload, recv_ns); })) {
        }
    }

    std::thread thread_;
    std::thread pub_thread_;

    uint64_t dropped_frames_{0};
};

}  // namespace bpt::md_gateway::adapter
