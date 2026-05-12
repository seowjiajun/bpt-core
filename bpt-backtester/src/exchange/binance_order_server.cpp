#include "backtester/exchange/binance_order_server.h"

#include <algorithm>
#include <atomic>
#include <boost/asio/post.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/json.hpp>
#include <bpt_common/logging.h>
#include <deque>
#include <format>
#include <sstream>
#include <string>
#include <unordered_map>

namespace beast = boost::beast;
namespace http = beast::http;
namespace ws = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace bpt::backtester::exchange {

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string dbl(double v) {
    return std::format("{:.10g}", v);
}

static std::unordered_map<std::string, std::string> parse_query(const std::string& qs) {
    std::unordered_map<std::string, std::string> out;
    std::istringstream ss(qs);
    std::string tok;
    while (std::getline(ss, tok, '&')) {
        auto eq = tok.find('=');
        if (eq != std::string::npos)
            out.emplace(tok.substr(0, eq), tok.substr(eq + 1));
    }
    return out;
}

static std::string format_execution_report(const matching::FillReport& fill, uint64_t order_id_int) {
    namespace json = boost::json;
    std::string side = (fill.side == matching::OrderSide::BUY) ? "BUY" : "SELL";
    std::string status = fill.is_fully_filled ? "FILLED" : (fill.cumulative_fill_qty > 0 ? "PARTIALLY_FILLED" : "NEW");
    std::string type = (fill.order_type == matching::OrderType::MARKET)      ? "MARKET"
                       : (fill.order_type == matching::OrderType::POST_ONLY) ? "LIMIT_MAKER"
                                                                             : "LIMIT";

    json::object obj;
    obj["e"] = "executionReport";
    obj["E"] = static_cast<int64_t>(fill.simulation_ts / 1'000'000);
    obj["s"] = fill.symbol;
    obj["c"] = fill.client_order_id;
    obj["S"] = side;
    obj["o"] = type;
    obj["q"] = dbl(fill.original_qty);
    obj["p"] = dbl(fill.order_price);
    obj["f"] = "GTC";
    obj["l"] = dbl(fill.last_fill_qty);
    obj["z"] = dbl(fill.cumulative_fill_qty);
    obj["L"] = dbl(fill.last_fill_price);
    obj["X"] = status;
    obj["i"] = static_cast<int64_t>(order_id_int);
    obj["T"] = static_cast<int64_t>(fill.simulation_ts / 1'000'000);
    return json::serialize(obj);
}

static std::string format_order_response(const matching::OpenOrder& order, uint64_t order_id_int, uint64_t sim_ts) {
    namespace json = boost::json;
    std::string side = (order.side == matching::OrderSide::BUY) ? "BUY" : "SELL";
    std::string type = (order.type == matching::OrderType::MARKET)      ? "MARKET"
                       : (order.type == matching::OrderType::POST_ONLY) ? "LIMIT_MAKER"
                                                                        : "LIMIT";
    std::string status =
        (order.filled_qty >= order.quantity) ? "FILLED" : (order.filled_qty > 0 ? "PARTIALLY_FILLED" : "NEW");

    json::object obj;
    obj["symbol"] = order.symbol;
    obj["orderId"] = static_cast<int64_t>(order_id_int);
    obj["clientOrderId"] = order.client_order_id;
    obj["transactTime"] = static_cast<int64_t>(sim_ts / 1'000'000);
    obj["price"] = dbl(order.price);
    obj["origQty"] = dbl(order.quantity);
    obj["executedQty"] = dbl(order.filled_qty);
    obj["status"] = status;
    obj["type"] = type;
    obj["side"] = side;
    return json::serialize(obj);
}

// ── BinanceOrderSession ───────────────────────────────────────────────────────

// A single connection that starts life as HTTP and may upgrade to a WebSocket
// user-data stream.  After the upgrade the session is purely a push channel.
class BinanceOrderSession : public std::enable_shared_from_this<BinanceOrderSession> {
public:
    explicit BinanceOrderSession(tcp::socket socket,
                                 matching::MatchingEngine& engine,
                                 std::atomic<uint64_t>& order_id_seq)
        : socket_(std::move(socket)),
          engine_(engine),
          order_id_seq_(order_id_seq) {}

    void run() { do_read_http(); }

    // Queues a message on the WS user-data channel.  No-op if not upgraded.
    void push(std::shared_ptr<std::string> msg) {
        if (!ws_upgraded_)
            return;
        bool idle = write_queue_.empty();
        write_queue_.push_back(std::move(msg));
        if (idle)
            do_ws_write();
    }

    bool closed() const { return closed_; }
    bool is_ws() const { return ws_upgraded_; }

private:
    // ── HTTP phase ──────────────────────────────────────────────────────────

    void do_read_http() {
        req_ = {};
        http::async_read(socket_, buf_, req_, [self = shared_from_this()](beast::error_code ec, std::size_t) {
            self->on_read_http(ec);
        });
    }

    void on_read_http(beast::error_code ec) {
        if (ec) {
            closed_ = true;
            return;
        }

        // WebSocket upgrade?
        if (ws::is_upgrade(req_)) {
            ws_stream_ = std::make_unique<ws::stream<tcp::socket>>(std::move(socket_));
            ws_stream_->async_accept(req_, [self = shared_from_this()](beast::error_code ec2) {
                if (ec2) {
                    self->closed_ = true;
                    return;
                }
                self->ws_upgraded_ = true;
                bpt::common::log::info("[BinanceOrderServer] User-data WS connected");
                // WS sessions are purely outbound; nothing to read.
            });
            return;
        }

        route_http();
    }

    void route_http() {
        auto method = req_.method();
        std::string target(req_.target());
        auto query_sep = target.find('?');
        std::string path = (query_sep == std::string::npos) ? target : target.substr(0, query_sep);
        std::string query = (query_sep == std::string::npos) ? "" : target.substr(query_sep + 1);

        namespace json = boost::json;
        http::response<http::string_body> res{http::status::ok, req_.version()};
        res.set(http::field::content_type, "application/json");
        res.keep_alive(false);

        if (path == "/api/v3/userDataStream") {
            if (method == http::verb::post) {
                json::object obj;
                obj["listenKey"] = "backtester-stream";
                res.body() = json::serialize(obj);
            }
            // PUT (keepalive) and DELETE: just 200 OK with empty body
        } else if (path == "/api/v3/order") {
            if (method == http::verb::post) {
                handle_new_order(query.empty() ? std::string(req_.body()) : query, res);
            } else if (method == http::verb::delete_) {
                handle_cancel_order(query, res);
            }
        } else if (path == "/api/v3/openOrders") {
            res.body() = "[]";
        } else {
            res.result(http::status::not_found);
            res.body() = R"({"code":-1121,"msg":"Invalid symbol."})";
        }

        res.prepare_payload();
        http::async_write(socket_, res, [self = shared_from_this()](beast::error_code ec, std::size_t) {
            self->closed_ = true;  // HTTP: close after response
            if (ec)
                return;
        });
    }

    void handle_new_order(const std::string& body, http::response<http::string_body>& res) {
        auto params = parse_query(body);

        matching::OpenOrder order;
        order.exchange = "BINANCE";
        order.symbol = params.count("symbol") ? params["symbol"] : "";
        order.client_order_id = params.count("newClientOrderId") ? params["newClientOrderId"] : "";
        const std::string type_str = params.count("type") ? params["type"] : "LIMIT";
        if (type_str == "MARKET")
            order.type = matching::OrderType::MARKET;
        else if (type_str == "LIMIT_MAKER")
            order.type = matching::OrderType::POST_ONLY;
        else
            order.type = matching::OrderType::LIMIT;
        order.side =
            (params.count("side") && params["side"] == "SELL") ? matching::OrderSide::SELL : matching::OrderSide::BUY;
        order.quantity = params.count("quantity") ? std::stod(params["quantity"]) : 0.0;
        order.price = params.count("price") ? std::stod(params["price"]) : 0.0;

        uint64_t oid = ++order_id_seq_;
        order.order_id = std::to_string(oid);

        auto filled = engine_.submit_order(std::move(order));

        // Binance rejects POST_ONLY (LIMIT_MAKER) orders that would cross
        // with HTTP 400 + code -2010 ("Order would immediately match and
        // take" — the documented LIMIT_MAKER post-only failure path).
        // The OGW binance_exec_decoder routes 4xx responses with that
        // code to a REJECTED ExecReport.
        if (filled.rejected) {
            res.result(http::status::bad_request);
            namespace json = boost::json;
            json::object err;
            err["code"] = -2010;
            err["msg"] = "Order would immediately match and take.";
            res.body() = json::serialize(err);
            return;
        }
        res.body() = format_order_response(filled, oid, filled.submitted_ts);
    }

    void handle_cancel_order(const std::string& query, http::response<http::string_body>& res) {
        auto params = parse_query(query);
        std::string symbol = params.count("symbol") ? params["symbol"] : "";
        std::string order_id = params.count("orderId") ? params["orderId"] : "";

        bool ok = engine_.cancel_order("BINANCE", symbol, order_id);
        if (!ok) {
            res.result(http::status::bad_request);
            res.body() = R"({"code":-2011,"msg":"Unknown order sent."})";
        } else {
            namespace json = boost::json;
            json::object obj;
            obj["orderId"] = std::stoll(order_id);
            obj["symbol"] = symbol;
            obj["status"] = "CANCELED";
            res.body() = json::serialize(obj);
        }
    }

    // ── WebSocket write phase ───────────────────────────────────────────────

    void do_ws_write() {
        if (write_queue_.empty() || closed_ || !ws_stream_)
            return;
        ws_stream_->async_write(net::buffer(*write_queue_.front()),
                                [self = shared_from_this()](beast::error_code ec, std::size_t) {
                                    if (ec) {
                                        self->closed_ = true;
                                        return;
                                    }
                                    self->write_queue_.pop_front();
                                    self->do_ws_write();
                                });
    }

    // ── State ───────────────────────────────────────────────────────────────

    tcp::socket socket_;
    beast::flat_buffer buf_;
    http::request<http::string_body> req_;
    std::unique_ptr<ws::stream<tcp::socket>> ws_stream_;
    std::deque<std::shared_ptr<std::string>> write_queue_;
    matching::MatchingEngine& engine_;
    std::atomic<uint64_t>& order_id_seq_;
    bool ws_upgraded_{false};
    bool closed_{false};
};

// ── BinanceOrderServer ────────────────────────────────────────────────────────

BinanceOrderServer::BinanceOrderServer(uint16_t port, matching::MatchingEngine& engine)
    : port_(port),
      engine_(engine),
      acceptor_(ioc_) {}

BinanceOrderServer::~BinanceOrderServer() {
    stop();
}

void BinanceOrderServer::start() {
    tcp::endpoint ep{tcp::v4(), port_};
    acceptor_.open(ep.protocol());
    acceptor_.set_option(net::socket_base::reuse_address(true));
    acceptor_.bind(ep);
    acceptor_.listen();
    bpt::common::log::info("[BinanceOrderServer] Listening on port {}", port_);
    do_accept();
    thread_ = std::thread([this] { ioc_.run(); });
}

void BinanceOrderServer::stop() {
    net::post(ioc_, [this] { acceptor_.close(); });
    ioc_.stop();
    if (thread_.joinable())
        thread_.join();
}

void BinanceOrderServer::do_accept() {
    acceptor_.async_accept([this](beast::error_code ec, tcp::socket socket) {
        if (!ec) {
            sessions_.erase(
                std::remove_if(sessions_.begin(), sessions_.end(), [](const auto& s) { return s->closed(); }),
                sessions_.end());
            auto session = std::make_shared<BinanceOrderSession>(std::move(socket), engine_, order_id_seq_);
            sessions_.push_back(session);
            session->run();
        }
        if (acceptor_.is_open())
            do_accept();
    });
}

void BinanceOrderServer::push_fill(const matching::FillReport& fill) {
    // Derive integer order ID from string (we generated it as to_string of the counter).
    uint64_t oid = 0;
    try {
        oid = std::stoull(fill.order_id);
    } catch (...) {
    }

    auto msg = std::make_shared<std::string>(format_execution_report(fill, oid));

    net::post(ioc_, [this, msg]() {
        sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(), [](const auto& s) { return s->closed(); }),
                        sessions_.end());
        for (const auto& s : sessions_)
            if (s->is_ws())
                s->push(msg);
    });
}

}  // namespace bpt::backtester::exchange
