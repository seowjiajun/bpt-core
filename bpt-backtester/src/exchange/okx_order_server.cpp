#include "backtester/exchange/okx_order_server.h"

#include <algorithm>
#include <atomic>
#include <boost/asio/post.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/json.hpp>
#include <bpt_common/logging.h>
#include <deque>
#include <format>
#include <string>

namespace beast = boost::beast;
namespace ws = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace bpt::backtester::exchange {

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string dbl(double v) {
    return std::format("{:.10g}", v);
}

// OKX execution report pushed on the "orders" channel.
// Backtester only emits the fill state — OrderGateway's adapter handles fills only
// (no ACKED/PARTIAL lifecycle needed).
static std::string format_execution_report(const matching::FillReport& fill, uint64_t order_id_int) {
    namespace json = boost::json;
    std::string side = (fill.side == matching::OrderSide::BUY) ? "buy" : "sell";
    std::string state = fill.is_fully_filled ? "filled" : "partially_filled";
    std::string ts = std::to_string(fill.simulation_ts / 1'000'000);
    // Fee is negative for taker (OKX convention: negative = cost)
    double fee_val = -(fill.last_fill_qty * fill.last_fill_price * 0.0005);

    std::string ord_type = (fill.order_type == matching::OrderType::MARKET)      ? "market"
                           : (fill.order_type == matching::OrderType::POST_ONLY) ? "post_only"
                                                                                 : "limit";

    json::object item;
    item["instId"] = fill.symbol;
    item["clOrdId"] = fill.client_order_id;
    item["ordId"] = std::to_string(order_id_int);
    item["side"] = side;
    item["ordType"] = ord_type;
    item["px"] = dbl(fill.order_price);
    item["sz"] = dbl(fill.original_qty);
    item["fillSz"] = dbl(fill.last_fill_qty);
    item["fillPx"] = dbl(fill.last_fill_price);
    item["accFillSz"] = dbl(fill.cumulative_fill_qty);
    item["fee"] = dbl(fee_val);
    item["feeCcy"] = "USDT";
    item["state"] = state;
    item["uTime"] = ts;

    json::object arg;
    arg["channel"] = "orders";

    json::object root;
    root["arg"] = std::move(arg);
    root["data"] = json::array{std::move(item)};
    return json::serialize(root);
}

// ── OkxOrderSession ───────────────────────────────────────────────────────────

// OKX uses a single WS channel for both order submission (incoming) and
// execution reports (outgoing).  OrderGateway connects and sends op messages.
class OkxOrderSession : public std::enable_shared_from_this<OkxOrderSession> {
public:
    explicit OkxOrderSession(tcp::socket socket, matching::MatchingEngine& engine, std::atomic<uint64_t>& order_id_seq)
        : ws_(std::move(socket)),
          engine_(engine),
          order_id_seq_(order_id_seq) {}

    void run() {
        ws_.async_accept([self = shared_from_this()](beast::error_code ec) { self->on_accept(ec); });
    }

    void send(std::shared_ptr<std::string> msg) {
        bool idle = write_queue_.empty();
        write_queue_.push_back(std::move(msg));
        if (idle)
            do_write();
    }

    bool closed() const { return closed_; }

private:
    void on_accept(beast::error_code ec) {
        if (ec) {
            bpt::common::log::warn("[OkxOrderServer] accept error: {}", ec.message());
            closed_ = true;
            return;
        }
        bpt::common::log::info("[OkxOrderServer] WS connected");
        do_read();
    }

    void do_read() {
        ws_.async_read(buf_, [self = shared_from_this()](beast::error_code ec, std::size_t) { self->on_read(ec); });
    }

    // Handles incoming op messages from OrderGateway:
    //   {"op":"order","args":[{"instId":"BTC-USDT-SWAP","clOrdId":"G42","side":"buy",
    //                          "ordType":"limit","px":"30000","sz":"1"}]}
    //   {"op":"cancel-order","args":[{"instId":"BTC-USDT-SWAP","clOrdId":"G42"}]}
    void on_read(beast::error_code ec) {
        if (ec) {
            closed_ = true;
            return;
        }

        std::string text = beast::buffers_to_string(buf_.data());
        buf_.consume(buf_.size());

        // OrderGateway sends plain-text "ping" keepalives — respond and continue.
        if (text == "ping") {
            send(std::make_shared<std::string>("pong"));
            do_read();
            return;
        }

        try {
            auto val = boost::json::parse(text);
            auto& obj = val.as_object();
            std::string op = std::string(obj["op"].as_string());

            if (op == "order") {
                handle_new_order(obj["args"].as_array()[0].as_object());
            } else if (op == "cancel-order") {
                handle_cancel_order(obj["args"].as_array()[0].as_object());
            } else if (op == "login") {
                // OKX private channel requires login — ack it immediately
                boost::json::object ack;
                ack["event"] = "login";
                ack["code"] = "0";
                ack["msg"] = "";
                send(std::make_shared<std::string>(boost::json::serialize(ack)));
            } else if (op == "subscribe") {
                // OrderGateway subscribes to the "orders" channel after login — ack silently.
                (void)op;
            }
        } catch (const std::exception& e) {
            bpt::common::log::warn("[OkxOrderServer] parse error: {}", e.what());
        }

        do_read();
    }

    void handle_new_order(const boost::json::object& args) {
        namespace json = boost::json;

        matching::OpenOrder order;
        order.exchange = "OKX";
        order.symbol = std::string(args.at("instId").as_string());
        order.client_order_id = std::string(args.at("clOrdId").as_string());

        std::string ord_type = std::string(args.at("ordType").as_string());
        if (ord_type == "market")
            order.type = matching::OrderType::MARKET;
        else if (ord_type == "post_only")
            order.type = matching::OrderType::POST_ONLY;
        else
            order.type = matching::OrderType::LIMIT;

        std::string side_str = std::string(args.at("side").as_string());
        order.side = (side_str == "sell") ? matching::OrderSide::SELL : matching::OrderSide::BUY;

        order.quantity = std::stod(std::string(args.at("sz").as_string()));
        if (args.contains("px"))
            order.price = std::stod(std::string(args.at("px").as_string()));

        uint64_t oid = ++order_id_seq_;
        order.order_id = std::to_string(oid);

        const auto submitted = engine_.submit_order(std::move(order));

        // OKX place-order response — the per-order data entry carries
        // its own sCode (success code) distinct from the envelope code.
        // POST_ONLY rejections set sCode=51008 (post-only would have
        // crossed) per OKX's docs; the OGW okx_exec_decoder routes any
        // non-"0" sCode to a REJECTED ExecReport.
        json::object ack;
        ack["id"] = std::string(args.contains("clOrdId") ? std::string(args.at("clOrdId").as_string()) : "");
        ack["op"] = "order";
        ack["code"] = "0";
        ack["msg"] = "";
        json::object data;
        data["clOrdId"] = submitted.client_order_id;
        data["ordId"] = std::to_string(oid);
        if (submitted.rejected) {
            data["sCode"] = "51008";
            data["sMsg"] = "Order would immediately match — post_only set.";
        } else {
            data["sCode"] = "0";
            data["sMsg"] = "";
        }
        ack["data"] = json::array{std::move(data)};
        send(std::make_shared<std::string>(json::serialize(ack)));
    }

    void handle_cancel_order(const boost::json::object& args) {
        namespace json = boost::json;

        std::string symbol = std::string(args.at("instId").as_string());
        std::string cl_ord_id = args.contains("clOrdId") ? std::string(args.at("clOrdId").as_string()) : "";
        std::string ord_id = args.contains("ordId") ? std::string(args.at("ordId").as_string()) : cl_ord_id;

        bool ok = engine_.cancel_order("OKX", symbol, ord_id);

        json::object ack;
        ack["op"] = "cancel-order";
        json::object data;
        data["clOrdId"] = cl_ord_id;
        data["ordId"] = ord_id;
        if (ok) {
            ack["code"] = "0";
            ack["msg"] = "";
            data["sCode"] = "0";
            data["sMsg"] = "";
        } else {
            ack["code"] = "1";
            ack["msg"] = "Order does not exist";
            data["sCode"] = "51400";
            data["sMsg"] = "Cancellation failed as order does not exist";
        }
        ack["data"] = json::array{std::move(data)};
        send(std::make_shared<std::string>(json::serialize(ack)));
    }

    void do_write() {
        if (write_queue_.empty() || closed_)
            return;
        ws_.async_write(net::buffer(*write_queue_.front()),
                        [self = shared_from_this()](beast::error_code ec, std::size_t) {
                            if (ec) {
                                self->closed_ = true;
                                return;
                            }
                            self->write_queue_.pop_front();
                            self->do_write();
                        });
    }

    ws::stream<tcp::socket> ws_;
    beast::flat_buffer buf_;
    std::deque<std::shared_ptr<std::string>> write_queue_;
    matching::MatchingEngine& engine_;
    std::atomic<uint64_t>& order_id_seq_;
    bool closed_{false};
};

// ── OkxOrderServer ────────────────────────────────────────────────────────────

OkxOrderServer::OkxOrderServer(uint16_t port, matching::MatchingEngine& engine)
    : port_(port),
      engine_(engine),
      acceptor_(ioc_) {}

OkxOrderServer::~OkxOrderServer() {
    stop();
}

void OkxOrderServer::start() {
    tcp::endpoint ep{tcp::v4(), port_};
    acceptor_.open(ep.protocol());
    acceptor_.set_option(net::socket_base::reuse_address(true));
    acceptor_.bind(ep);
    acceptor_.listen();
    bpt::common::log::info("[OkxOrderServer] Listening on port {}", port_);
    do_accept();
    thread_ = std::thread([this] { ioc_.run(); });
}

void OkxOrderServer::stop() {
    net::post(ioc_, [this] { acceptor_.close(); });
    ioc_.stop();
    if (thread_.joinable())
        thread_.join();
}

void OkxOrderServer::do_accept() {
    acceptor_.async_accept([this](beast::error_code ec, tcp::socket socket) {
        if (!ec) {
            sessions_.erase(
                std::remove_if(sessions_.begin(), sessions_.end(), [](const auto& s) { return s->closed(); }),
                sessions_.end());
            auto session = std::make_shared<OkxOrderSession>(std::move(socket), engine_, order_id_seq_);
            sessions_.push_back(session);
            session->run();
        }
        if (acceptor_.is_open())
            do_accept();
    });
}

void OkxOrderServer::push_fill(const matching::FillReport& fill) {
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
            s->send(msg);
    });
}

}  // namespace bpt::backtester::exchange
