#pragma once

// yggdrasil/ws/run_loop.h — WebSocket read/send/ping run-loop base.
//
// Handles the boilerplate that every long-lived authenticated WS client
// needs:
//   - Read loop with configurable per-read timeout so a silent server
//     disconnect unblocks via `beast::error::timeout` instead of hanging
//     forever.
//   - Thread-safe send() callable from any thread (ping thread, order-
//     entry thread, etc.).
//   - Optional ping heartbeat thread with configurable cadence + payload.
//   - Clean shutdown on stop_flag and on exceptions thrown from any hook.
//
// What subclasses provide:
//   - on_handshake_complete():  send a login/auth message if the
//     exchange requires it. Called exactly once after the WS handshake
//     completes and BEFORE the read loop starts.
//   - on_frame(payload, recv_ns): called for each inbound text/binary
//     frame. recv_ns is WallClock::now_ns() at receive time.
//   - ping_config(): return a cadence + payload-factory if the exchange
//     expects application-level pings (OKX, HL). Return nullopt for
//     exchanges with their own heartbeat protocol (Deribit).
//
// What's intentionally NOT in here:
//   - Exchange auth payload construction (each exchange is different).
//   - Request/response correlation (Hyperliquid-style) — kept in its own
//     client because it needs concurrent access to the raw stream, which
//     doesn't fit the single-owner-stream pattern this class uses.

#include "bpt_common/logging.h"
#include "bpt_common/util/tsc_clock.h"
#include "bpt_common/ws/ws_connect.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

namespace bpt::common::ws {

struct PingConfig {
    std::chrono::milliseconds interval;
    // Called on each tick to produce the ping payload. A function (not a
    // fixed string) so exchanges that need a sequence number or nonce
    // can generate fresh payloads per ping.
    std::function<std::string()> payload;
};

class RunLoop {
public:
    RunLoop() = default;
    virtual ~RunLoop() = default;

    RunLoop(const RunLoop&) = delete;
    RunLoop& operator=(const RunLoop&) = delete;

    // Run the read + ping loop against the supplied (already-connected)
    // stream. Returns cleanly when stop_flag goes true; throws on any
    // WS error so the caller's outer reconnect loop can catch + retry.
    //
    // connected is set true after on_handshake_complete() returns, and
    // false on exit (normal or exceptional).
    void run(AnyWsStream ws,
             std::atomic<bool>& stop_flag,
             std::atomic<bool>& connected,
             std::chrono::milliseconds read_timeout = std::chrono::seconds(30));

    // Thread-safe write. Returns false if the stream is not currently
    // connected (before run() starts, after run() returns, or during a
    // failed state). Safe to call from a dedicated ping thread or from
    // the adapter's command-handling thread.
    bool send(const std::string& msg);

protected:
    virtual void on_handshake_complete() {}
    virtual void on_frame(std::string_view payload, uint64_t recv_ns) = 0;
    virtual std::optional<PingConfig> ping_config() const { return std::nullopt; }

private:
    void ping_thread_fn(std::atomic<bool>& stop_flag, PingConfig cfg);

    std::mutex send_mu_;
    // Valid only between the `text(true)` call in run() and the close
    // at end-of-run; guarded by send_mu_. A null pointer means send()
    // short-circuits, which is the correct behavior during reconnect.
    AnyWsStream* ws_ = nullptr;
};

inline void RunLoop::run(AnyWsStream ws,
                         std::atomic<bool>& stop_flag,
                         std::atomic<bool>& connected,
                         std::chrono::milliseconds read_timeout) {
    namespace beast = boost::beast;

    ws.text(true);
    {
        std::lock_guard<std::mutex> lk(send_mu_);
        ws_ = &ws;
    }

    // Ensure the send pointer is cleared before this function returns,
    // whether via stop_flag (clean exit) or exception (WS error). This
    // guarantees any concurrent send() called after run() exits will
    // return false instead of writing into freed memory.
    struct SendGuard {
        RunLoop* self;
        ~SendGuard() {
            std::lock_guard<std::mutex> lk(self->send_mu_);
            self->ws_ = nullptr;
        }
    } send_guard{this};

    // Run the subclass auth hook while we're already tracking the stream
    // so on_handshake_complete() can call send() to transmit a login
    // message if it needs to.
    on_handshake_complete();

    connected.store(true, std::memory_order_relaxed);

    // Ping thread lifecycle is scoped to run(): constructed before the
    // read loop, joined on exit (normal or exceptional).
    std::thread ping_thread;
    std::atomic<bool> ping_stop{false};
    if (auto cfg = ping_config()) {
        ping_thread = std::thread([this, &ping_stop, &stop_flag, cfg = *cfg] {
            // Use a combined stop: the outer stop_flag OR our own ping_stop
            // (set on run() exit). The ping thread observes both to ensure
            // it wakes on BOTH shutdown paths.
            while (!stop_flag.load(std::memory_order_relaxed) &&
                   !ping_stop.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(cfg.interval);
                if (stop_flag.load(std::memory_order_relaxed) ||
                    ping_stop.load(std::memory_order_relaxed)) break;
                try {
                    send(cfg.payload());
                } catch (...) {
                    // Ping write errors are expected on disconnect —
                    // the read loop will observe the same failure and
                    // propagate. Swallow here so the ping thread exits
                    // cleanly rather than terminating via unhandled
                    // exception.
                    return;
                }
            }
        });
    }

    // Similarly scope the ping thread join to run exit.
    struct PingGuard {
        std::thread* t;
        std::atomic<bool>* stop;
        ~PingGuard() {
            stop->store(true, std::memory_order_relaxed);
            if (t->joinable()) t->join();
        }
    } ping_guard{&ping_thread, &ping_stop};

    try {
        beast::flat_buffer buf;
        while (!stop_flag.load(std::memory_order_relaxed)) {
            ws.expires_after(read_timeout);
            beast::error_code ec;
            ws.read(buf, ec);

            if (ec == beast::error::timeout) {
                // No frame within read_timeout. Drain and loop — this
                // gives stop_flag a chance to be observed on quiet
                // connections and is the main mechanism that prevents
                // the loop from hanging on a silently-dead connection.
                buf.consume(buf.size());
                continue;
            }
            if (ec) throw beast::system_error(ec);

            const uint64_t recv_ns = bpt::common::util::WallClock::now_ns();
            std::string_view payload(
                static_cast<const char*>(buf.data().data()),
                buf.data().size());
            on_frame(payload, recv_ns);
            buf.consume(buf.size());
        }
    } catch (...) {
        connected.store(false, std::memory_order_relaxed);
        // send_guard clears ws_, ping_guard joins the ping thread.
        throw;
    }

    connected.store(false, std::memory_order_relaxed);
    ws.close(boost::beast::websocket::close_code::normal);
}

inline bool RunLoop::send(const std::string& msg) {
    std::lock_guard<std::mutex> lk(send_mu_);
    if (ws_ == nullptr) return false;
    namespace net = boost::asio;
    ws_->write(net::buffer(msg));
    return true;
}

}  // namespace bpt::common::ws
