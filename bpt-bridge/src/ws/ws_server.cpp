#include "bridge/ws/ws_server.h"

#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <bpt_common/logging.h>
#include <bpt_common/util/thread_name.h>
#include <deque>
#include <nlohmann/json.hpp>

namespace bpt::bridge {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace {
quill::Logger* kLog() {
    static quill::Logger* l = bpt::common::logging::get_logger("WsServer");
    return l;
}
quill::Logger* kWsSessionLog() {
    static quill::Logger* l = bpt::common::logging::get_logger("WsSession");
    return l;
}
}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
//  WsSession — one per connected client
// ─────────────────────────────────────────────────────────────────────────────
class WsSession : public std::enable_shared_from_this<WsSession> {
public:
    WsSession(tcp::socket&& socket, WsServer& server) : ws_(std::move(socket)), server_(server) {}

    void run() {
        ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
        ws_.set_option(websocket::stream_base::decorator(
            [](websocket::response_type& res) { res.set(beast::http::field::server, "bpt-bridge"); }));

        ws_.async_accept([self = shared_from_this()](beast::error_code ec) {
            if (ec) {
                bpt::common::log::warn(kWsSessionLog(), "accept failed: {}", ec.message());
                return;
            }
            self->server_.add_session(self);
            self->do_read();
        });
    }

    // Queue a message for writing.  Called from the IO thread via post().
    void enqueue(std::shared_ptr<const std::string> msg) {
        queue_.push_back(std::move(msg));
        if (queue_.size() == 1)
            do_write();
    }

    void close() {
        beast::error_code ec;
        ws_.close(websocket::close_code::normal, ec);
    }

private:
    void do_read() {
        ws_.async_read(buffer_, [self = shared_from_this()](beast::error_code ec, std::size_t bytes) {
            if (ec == websocket::error::closed || ec) {
                self->server_.remove_session(self);
                return;
            }
            self->handle_inbound(bytes);
            self->buffer_.consume(self->buffer_.size());
            self->do_read();
        });
    }

    void handle_inbound(std::size_t bytes) {
        if (bytes == 0)
            return;
        try {
            auto data = beast::buffers_to_string(buffer_.data());
            auto j = nlohmann::json::parse(data);
            if (j.value("type", "") == "command") {
                std::string cmd = j.value("cmd", "");
                if (!cmd.empty()) {
                    bpt::common::log::info(kWsSessionLog(), "received command: {}", cmd);
                    server_.dispatch_command(cmd);
                }
            }
        } catch (const std::exception& e) {
            bpt::common::log::warn(kWsSessionLog(), "bad inbound message: {}", e.what());
        }
    }

    void do_write() {
        ws_.text(true);
        ws_.async_write(net::buffer(*queue_.front()),
                        [self = shared_from_this()](beast::error_code ec, std::size_t /*bytes*/) {
                            if (ec) {
                                self->server_.remove_session(self);
                                return;
                            }
                            self->queue_.pop_front();
                            if (!self->queue_.empty())
                                self->do_write();
                        });
    }

    websocket::stream<beast::tcp_stream> ws_;
    WsServer& server_;
    beast::flat_buffer buffer_;
    std::deque<std::shared_ptr<const std::string>> queue_;
};

// ─────────────────────────────────────────────────────────────────────────────
//  WsServer
// ─────────────────────────────────────────────────────────────────────────────
WsServer::WsServer(uint16_t port) : port_(port), acceptor_(io_ctx_) {}

WsServer::~WsServer() {
    stop();
}

void WsServer::start() {
    beast::error_code ec;
    tcp::endpoint endpoint{tcp::v4(), port_};

    acceptor_.open(endpoint.protocol(), ec);
    if (ec) {
        bpt::common::log::error(kLog(), "open: {}", ec.message());
        return;
    }

    acceptor_.set_option(net::socket_base::reuse_address(true), ec);
    acceptor_.bind(endpoint, ec);
    if (ec) {
        bpt::common::log::error(kLog(), "bind {}: {}", port_, ec.message());
        return;
    }

    acceptor_.listen(net::socket_base::max_listen_connections, ec);
    if (ec) {
        bpt::common::log::error(kLog(), "listen: {}", ec.message());
        return;
    }

    bpt::common::log::info(kLog(), "listening on :{}", port_);
    do_accept();

    io_thread_ = std::thread([this] {
        // WsServer doesn't carry venue; process comm already does
        // (bpt-bridge-<venue>). The thread name just marks its role.
        bpt::common::util::set_thread_name("bridge-ws");
        try {
            io_ctx_.run();
        } catch (const std::exception& e) {
            bpt::common::log::error(kLog(), "io_context crashed: {}", e.what());
        }
    });
}

void WsServer::stop() {
    if (!io_thread_.joinable())
        return;

    net::post(io_ctx_, [this] {
        beast::error_code ec;
        acceptor_.close(ec);
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        for (auto& s : sessions_)
            s->close();
        sessions_.clear();
    });

    io_ctx_.stop();
    if (io_thread_.joinable())
        io_thread_.join();
}

void WsServer::do_accept() {
    acceptor_.async_accept([this](beast::error_code ec, tcp::socket socket) {
        // operation_aborted is the only non-retryable error — it fires when
        // the acceptor is being closed during shutdown.  Anything else is a
        // transient per-connection error (client RST, handshake failure, etc.)
        // — log and keep accepting.
        if (ec == net::error::operation_aborted)
            return;

        if (ec) {
            bpt::common::log::warn(kLog(), "accept: {}", ec.message());
        } else {
            std::make_shared<WsSession>(std::move(socket), *this)->run();
        }

        // Re-arm unconditionally so a single bad client can't kill the loop.
        do_accept();
    });
}

void WsServer::publish(MsgKind kind, std::string message) {
    auto msg = std::make_shared<const std::string>(std::move(message));
    // Hop onto the IO thread so snapshot mutation and session enqueues are
    // always single-threaded — no mutex needed on the snapshot, only on the
    // sessions set (for stop() which runs on the calling thread).
    net::post(io_ctx_, [this, kind, msg] {
        update_snapshot(kind, msg);

        std::vector<std::shared_ptr<WsSession>> live;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            live.reserve(sessions_.size());
            for (auto& s : sessions_)
                live.push_back(s);
        }
        for (auto& s : live)
            s->enqueue(msg);
    });
}

void WsServer::update_snapshot(MsgKind kind, std::shared_ptr<const std::string> msg) {
    // Called only from the IO thread — no mutex on snapshot_.
    switch (kind) {
        case MsgKind::Session:
            snapshot_.session_msg = std::move(msg);
            break;
        case MsgKind::Status:
            snapshot_.status_msg = std::move(msg);
            break;
        case MsgKind::Tick:
            snapshot_.tick_msg = std::move(msg);
            break;
        case MsgKind::Position:
            snapshot_.position_msg = std::move(msg);
            break;
        case MsgKind::Toxicity:
            snapshot_.toxicity_msg = std::move(msg);
            break;
        case MsgKind::MarketColor:
            snapshot_.market_color_msg = std::move(msg);
            break;
        case MsgKind::Fill:
            snapshot_.fills.push_back(std::move(msg));
            while (snapshot_.fills.size() > Snapshot::kMaxFills)
                snapshot_.fills.pop_front();
            break;
    }
}

void WsServer::replay_snapshot_to(const std::shared_ptr<WsSession>& session) {
    // Logical replay order: session config → status → last tick → all fills
    // (oldest first) → latest position.  The frontend store handles messages
    // in any order, but this ordering matches what a live client would have
    // seen in chronological order.
    if (snapshot_.session_msg)
        session->enqueue(snapshot_.session_msg);
    if (snapshot_.status_msg)
        session->enqueue(snapshot_.status_msg);
    if (snapshot_.tick_msg)
        session->enqueue(snapshot_.tick_msg);
    for (auto& f : snapshot_.fills)
        session->enqueue(f);
    if (snapshot_.position_msg)
        session->enqueue(snapshot_.position_msg);
    if (snapshot_.toxicity_msg)
        session->enqueue(snapshot_.toxicity_msg);
    if (snapshot_.market_color_msg)
        session->enqueue(snapshot_.market_color_msg);
}

void WsServer::add_session(std::shared_ptr<WsSession> session) {
    // Called from the IO thread (WsSession::run's async_accept handler).
    // Replay the snapshot before adding the session to the live broadcast
    // set so the snapshot lands before any newly-broadcast message.
    replay_snapshot_to(session);

    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_.insert(std::move(session));
}

void WsServer::remove_session(const std::shared_ptr<WsSession>& session) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_.erase(session);
}

void WsServer::set_command_handler(CommandHandler h) {
    std::lock_guard<std::mutex> lock(cmd_mutex_);
    cmd_handler_ = std::move(h);
}

void WsServer::dispatch_command(const std::string& cmd) {
    // Snapshot under the lock so handler invocation runs without holding it.
    CommandHandler h;
    {
        std::lock_guard<std::mutex> lock(cmd_mutex_);
        h = cmd_handler_;
    }
    if (h)
        h(cmd);
}

}  // namespace bpt::bridge
