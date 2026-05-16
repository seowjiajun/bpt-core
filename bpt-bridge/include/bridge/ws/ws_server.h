#pragma once

#include "bridge/ws/i_broadcaster.h"
#include "bridge/ws/msg_kind.h"

#include <boost/asio.hpp>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

namespace bpt::bridge {

class WsSession;

// Minimal broadcast-style WebSocket server with session replay.
//
// Clients that connect mid-run get a snapshot burst (session, status, latest
// tick, recent fills, latest position) before they start receiving live
// messages, so the UI state is consistent regardless of join time.
//
// Threading: the snapshot is mutated only on the io_context thread (via
// net::post).  The sessions set is protected by sessions_mutex_ so that
// any thread can check connection count / close sessions on shutdown.
class WsServer : public ws::IBroadcaster {
public:
    explicit WsServer(uint16_t port);
    ~WsServer() override;

    WsServer(const WsServer&) = delete;
    WsServer& operator=(const WsServer&) = delete;

    void start();
    void stop();

    // Publish a typed message.  Updates the replay snapshot and broadcasts
    // to every connected session.  Thread-safe; callable from any thread.
    void publish(MsgKind kind, std::string message) override;

    // IBroadcaster: install the command-from-client handler. WsServer fires
    // this on its IO thread.  Lifetime is managed by BridgeService:
    // service calls set_command_handler(nullptr) before shutdown.
    void set_command_handler(CommandHandler h) override;

    // Internal — invoked by WsSession on the IO thread when a command frame
    // arrives. Forwards to the handler installed via set_command_handler.
    void dispatch_command(const std::string& cmd);

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
        std::shared_ptr<const std::string> toxicity_msg;
        std::shared_ptr<const std::string> market_color_msg;
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

    // Command handler installed via set_command_handler().  Read-only on the
    // IO thread once start() returns; written from the main thread before
    // start() and during shutdown — those two windows don't race the IO
    // thread because the IO thread isn't running yet (start) or has been
    // joined (after stop). Held under cmd_mutex_ defensively.
    std::mutex cmd_mutex_;
    CommandHandler cmd_handler_;
};

}  // namespace bpt::bridge
