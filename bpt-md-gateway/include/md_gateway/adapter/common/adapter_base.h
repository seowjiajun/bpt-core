#pragma once

#include "md_gateway/adapter/common/i_adapter.h"
#include "md_gateway/adapter/common/subscription_map.h"
#include "md_gateway/config/settings.h"
#include "md_gateway/md/md_validator.h"
#include "md_gateway/md/validating_publisher.h"
#include "md_gateway/messaging/i_md_publisher.h"
#include "md_gateway/recorder/raw_spool.h"

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <bpt_common/util/spsc_queue.h>
#include <bpt_common/ws/ws_connect.h>

namespace bpt::md_gateway::adapter {

// Base class for all exchange market-data adapters.
//
// Owns the common lifecycle state: io_context, ssl_context, subscription map,
// stop flag, and two threads:
//   IO thread      — WebSocket receive, stamps recv_ns, pushes raw frames to frame_queue_.
//   Publisher thread — drains frame_queue_, calls parse_frame() → Aeron offer.
//
// Subclasses implement:
//   connect_and_subscribe() — open WS, send initial subscribe frames.
//   read_loop(ws)           — receive loop; call push_frame() for each data frame.
//   parse_frame(payload, recv_ns) — calls the exchange parser + md_pub_ publish.
//
// The reconnect loop in run() calls connect_and_subscribe() + read_loop() in a
// tight try/catch.  Subclasses may override reconnect_delay() (default: 1 s).
class AdapterBase : public IAdapter {
public:
    // 512 slots × 16 KiB each ≈ 8 MiB per adapter.
    // 16 KiB covers the largest expected WS frame (Deribit book snapshot at
    // depth=255: ~15 KiB).  Increase SLOT_BYTES if larger frames are needed.
    static constexpr size_t QUEUE_CAPACITY = 512;
    static constexpr size_t SLOT_BYTES = 16384;
    using FrameQueue = bpt::common::util::SpscQueue<QUEUE_CAPACITY, SLOT_BYTES>;

    AdapterBase(const config::AdapterConfig& cfg,
                std::shared_ptr<messaging::IMdPublisher> md_pub,
                const config::RecordingConfig& recording = {});
    ~AdapterBase() override = default;

    void subscribe(uint64_t instrument_id, std::string symbol, uint8_t depth = 0) override;
    void unsubscribe(uint64_t instrument_id) override;
    void start() override;
    void stop() override;
    void set_topology(const bpt::common::util::Topology& topology) override { topology_ = &topology; }

    [[nodiscard]] uint64_t md_published_count() const noexcept override { return validating_pub_.published(); }
    [[nodiscard]] uint64_t validation_drop_count() const noexcept override { return validating_pub_.drops(); }
    [[nodiscard]] bool validation_drop_breaker_tripped() const noexcept override {
        return validating_pub_.breaker_tripped();
    }

protected:
    // How long to wait after a connection error before reconnecting.
    // Default: 1 s.  Override in derived class to change.
    virtual std::chrono::milliseconds reconnect_delay() const;

    // Establish the WebSocket connection and send all initial subscribe frames.
    // Return nullptr if no subscriptions exist yet (run() will retry after 100ms).
    virtual std::unique_ptr<bpt::common::ws::AnyWsStream> connect_and_subscribe() = 0;

    // Run the synchronous message-receive loop.  Call push_frame() for each
    // data message.  Throw on any fatal error to trigger a reconnect.
    virtual void read_loop(bpt::common::ws::AnyWsStream& ws) = 0;

    // Called by the publisher thread for each frame dequeued from frame_queue_.
    // Implementations call their parser then md_pub_ publish methods.
    virtual void parse_frame(std::string_view payload, uint64_t recv_ns) = 0;

    // Push a raw WS frame onto the SPSC queue.  Called from the IO thread.
    // Logs a warning (throttled) when the queue is full or the frame is oversized.
    void push_frame(std::string_view payload, uint64_t recv_ns) noexcept;

    // Tap raw frame to disk if recording is enabled. No-op otherwise.
    // Adapters call this at the START of on_frame() — BEFORE any keepalive
    // filter — so ping/pong are also captured (proof of liveness in the tape).
    void record_raw(std::string_view payload, uint64_t recv_ns) noexcept;

    // Periodic CHECKPOINT marker emission. Adapters call this from their
    // on_tick() callback. Cheap when disabled, ~1µs when enabled and writing.
    void maybe_checkpoint() noexcept;

    config::AdapterConfig cfg_;
    std::shared_ptr<messaging::IMdPublisher> md_pub_;
    // Optional CPU-affinity topology. Pointer (not reference) because
    // the base can be constructed before topology is known; set via
    // set_topology() before start(). nullptr = fall back to the legacy
    // cfg_.io_thread_cpu TOML knob.
    const bpt::common::util::Topology* topology_{nullptr};
    // validator_ must be declared before validating_pub_ (initializer-list order).
    md::MdValidator validator_;
    md::ValidatingPublisher validating_pub_;

    SubscriptionMap subs_;
    boost::asio::io_context ioc_;
    boost::asio::ssl::context ssl_ctx_;

    std::atomic<bool> stop_flag_{false};

    FrameQueue frame_queue_;

    // nullptr unless cfg's RecordingConfig.enabled = true. Owned by AdapterBase
    // because the IO thread is the sole writer; lifetime matches the adapter.
    std::unique_ptr<recorder::RawSpool> spool_;
    uint64_t checkpoint_interval_ns_{0};
    uint64_t last_checkpoint_ns_{0};

private:
    void run();
    void publish_loop();

    std::thread thread_;
    std::thread pub_thread_;

    uint64_t dropped_frames_{0};
};

}  // namespace bpt::md_gateway::adapter
