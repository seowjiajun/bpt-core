#pragma once

#include <boost/asio.hpp>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

namespace bridge {

class WsSession;

// Tagged message kinds — the server needs to know what kind of message it is
// so it can update its session-replay snapshot correctly.
enum class MsgKind : uint8_t {
    Session,    // latest wins
    Status,     // latest wins
    Tick,       // latest wins (per symbol — single-symbol for now)
    Fill,       // appended to rolling buffer
    Position,   // latest wins (per symbol)
    Order,      // not snapshotted — transient lifecycle events
};

// Minimal broadcast-style WebSocket server with session replay.
//
// Clients that connect mid-run get a snapshot burst (session, status, latest
// tick, recent fills, latest position) before they start receiving live
// messages, so the UI state is consistent regardless of join time.
//
// Threading: the snapshot is mutated only on the io_context thread (via
// net::post).  The sessions set is protected by sessions_mutex_ so that
// any thread can check connection count / close sessions on shutdown.
class WsServer {
public:
    explicit WsServer(uint16_t port);
    ~WsServer();

    WsServer(const WsServer&) = delete;
    WsServer& operator=(const WsServer&) = delete;

    void start();
    void stop();

    // Callback invoked on the IO thread when a connected client sends a
    // valid command (e.g. "halt", "resume").  The bridge's main() wires
    // this to publish on the control Aeron stream + broadcast status.
    std::function<void(const std::string& cmd)> on_command;

    // Publish a typed message.  Updates the replay snapshot and broadcasts
    // to every connected session.  Thread-safe; callable from any thread.
    void publish(MsgKind kind, std::string message);

    // Called by sessions on the io_context thread after the WebSocket
    // handshake completes.  Pushes the current snapshot to the new session
    // before registering it for future broadcasts.
    void add_session(std::shared_ptr<WsSession> session);
    void remove_session(const std::shared_ptr<WsSession>& session);

private:
    // ── IO-thread-only state ────────────────────────────────────────────────
    struct Snapshot {
        static constexpr std::size_t kMaxFills = 500;

        std::shared_ptr<const std::string> session_msg;
        std::shared_ptr<const std::string> status_msg;
        std::shared_ptr<const std::string> tick_msg;
        std::shared_ptr<const std::string> position_msg;
        std::deque<std::shared_ptr<const std::string>> fills;
    };

    void do_accept();
    void update_snapshot(MsgKind kind, std::shared_ptr<const std::string> msg);
    void replay_snapshot_to(const std::shared_ptr<WsSession>& session);

    uint16_t port_;
    boost::asio::io_context io_ctx_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::thread io_thread_;

    Snapshot snapshot_;  // IO-thread-only

    std::mutex sessions_mutex_;
    std::unordered_set<std::shared_ptr<WsSession>> sessions_;
};

}  // namespace bridge
