#include "backtester/exchange/okx_md_server.h"

#include "backtester/data/orderbook_record.h"
#include "backtester/data/trade_record.h"

#include <algorithm>
#include <boost/asio/post.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/json.hpp>
#include <deque>
#include <format>
#include <future>
#include <set>
#include <string>
#include <bpt_common/logging.h>

namespace beast = boost::beast;
namespace ws = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace bpt::backtester::exchange {

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string dbl(double v) {
    return std::format("{:.10g}", v);
}

// OKX subscription key: "<channel>|<instId>", e.g. "trades|BTC-USDT-SWAP".
static std::string sub_key(const std::string& channel, const std::string& inst_id) {
    return channel + "|" + inst_id;
}

static std::string format_trade(const data::TradeRecord& t) {
    namespace json = boost::json;

    std::string side = (t.side == data::TradeSide::BUY) ? "buy" : "sell";
    std::string ts = std::to_string(t.timestamp_ns / 1'000'000);

    json::object item;
    item["instId"] = t.symbol;
    item["px"] = dbl(t.price);
    item["sz"] = dbl(t.quantity);
    item["side"] = side;
    item["ts"] = ts;

    json::object arg;
    arg["channel"] = "trades";
    arg["instId"] = t.symbol;

    json::object root;
    root["arg"] = std::move(arg);
    root["data"] = json::array{std::move(item)};
    return json::serialize(root);
}

static std::string format_books5(const data::OrderBookRecord& ob) {
    namespace json = boost::json;

    json::array bids, asks;
    for (int i = 0; i < data::kOrderBookDepth; ++i) {
        // OKX books5 level: [price, size, liquidated_orders, order_count]
        bids.push_back(json::array{dbl(ob.bid_px[i]), dbl(ob.bid_sz[i]), "0", "1"});
        asks.push_back(json::array{dbl(ob.ask_px[i]), dbl(ob.ask_sz[i]), "0", "1"});
    }

    std::string ts = std::to_string(ob.timestamp_ns / 1'000'000);

    json::object item;
    item["bids"] = std::move(bids);
    item["asks"] = std::move(asks);
    item["ts"] = ts;

    json::object arg;
    arg["channel"] = "books5";
    arg["instId"] = ob.symbol;

    json::object root;
    root["arg"] = std::move(arg);
    root["data"] = json::array{std::move(item)};
    return json::serialize(root);
}

// bbo-tbt format: top-of-book only (one bid + one ask level).
// MdGateway subscribes with depth=0 → bbo-tbt channel.
static std::string format_bbo_tbt(const data::OrderBookRecord& ob) {
    namespace json = boost::json;

    std::string ts = std::to_string(ob.timestamp_ns / 1'000'000);

    json::object item;
    item["bids"] = json::array{json::array{dbl(ob.bid_px[0]), dbl(ob.bid_sz[0]), "0", "1"}};
    item["asks"] = json::array{json::array{dbl(ob.ask_px[0]), dbl(ob.ask_sz[0]), "0", "1"}};
    item["ts"] = ts;

    json::object arg;
    arg["channel"] = "bbo-tbt";
    arg["instId"] = ob.symbol;

    json::object root;
    root["arg"] = std::move(arg);
    root["data"] = json::array{std::move(item)};
    return json::serialize(root);
}

// ── OkxMdSession ──────────────────────────────────────────────────────────────

class OkxMdSession : public std::enable_shared_from_this<OkxMdSession> {
public:
    explicit OkxMdSession(tcp::socket socket) : ws_(std::move(socket)) {}

    void run() {
        ws_.async_accept([self = shared_from_this()](beast::error_code ec) { self->on_accept(ec); });
    }

    void send(std::shared_ptr<std::string> msg) {
        bool idle = write_queue_.empty();
        write_queue_.push_back(std::move(msg));
        if (idle)
            do_write();
    }

    bool is_subscribed(const std::string& channel, const std::string& inst_id) const {
        return subs_.count(sub_key(channel, inst_id)) > 0;
    }

    bool closed() const { return closed_; }

private:
    void on_accept(beast::error_code ec) {
        if (ec) {
            bpt::common::log::warn("[OkxMdServer] accept error: {}", ec.message());
            closed_ = true;
            return;
        }
        do_read();
    }

    void do_read() {
        ws_.async_read(buf_, [self = shared_from_this()](beast::error_code ec, std::size_t) { self->on_read(ec); });
    }

    // OKX subscribe message format:
    //   {"op":"subscribe","args":[{"channel":"trades","instId":"BTC-USDT-SWAP"}]}
    void on_read(beast::error_code ec) {
        if (ec) {
            closed_ = true;
            return;
        }

        std::string text = beast::buffers_to_string(buf_.data());
        buf_.consume(buf_.size());

        // MdGateway sends a plain-text "ping" (not JSON) for keepalives.
        if (text == "ping") {
            send(std::make_shared<std::string>("pong"));
            do_read();
            return;
        }

        try {
            auto val = boost::json::parse(text);
            auto& obj = val.as_object();
            std::string op = std::string(obj["op"].as_string());

            if (op == "subscribe" || op == "unsubscribe") {
                for (const auto& arg : obj["args"].as_array()) {
                    const auto& a = arg.as_object();
                    std::string channel = std::string(a.at("channel").as_string());
                    std::string inst_id = std::string(a.at("instId").as_string());
                    std::string key = sub_key(channel, inst_id);

                    if (op == "subscribe") {
                        subs_.insert(key);
                        // Confirm subscription
                        boost::json::object ack;
                        boost::json::object ack_arg;
                        ack_arg["channel"] = channel;
                        ack_arg["instId"] = inst_id;
                        ack["event"] = "subscribe";
                        ack["arg"] = std::move(ack_arg);
                        send(std::make_shared<std::string>(boost::json::serialize(ack)));
                        bpt::common::log::debug("[OkxMdServer] subscribed: {} {}", channel, inst_id);
                    } else {
                        subs_.erase(key);
                    }
                }
            } else if (op == "ping") {
                // JSON-format ping ({"op":"ping"}) — respond with plain pong
                send(std::make_shared<std::string>("pong"));
            }
        } catch (const std::exception& e) {
            bpt::common::log::warn("[OkxMdServer] parse error: {}", e.what());
        }

        do_read();
    }

    void do_write() {
        if (write_queue_.empty() || closed_)
            return;
        ws_.async_write(net::buffer(*write_queue_.front()),
                        [self = shared_from_this()](beast::error_code ec, std::size_t) { self->on_write(ec); });
    }

    void on_write(beast::error_code ec) {
        if (ec) {
            closed_ = true;
            return;
        }
        write_queue_.pop_front();
        do_write();
    }

    ws::stream<tcp::socket> ws_;
    beast::flat_buffer buf_;
    std::set<std::string> subs_;
    std::deque<std::shared_ptr<std::string>> write_queue_;
    bool closed_{false};
};

// ── OkxMdServer ───────────────────────────────────────────────────────────────

OkxMdServer::OkxMdServer(uint16_t port) : port_(port), acceptor_(ioc_) {}

OkxMdServer::~OkxMdServer() {
    stop();
}

void OkxMdServer::start() {
    tcp::endpoint ep{tcp::v4(), port_};
    acceptor_.open(ep.protocol());
    acceptor_.set_option(net::socket_base::reuse_address(true));
    acceptor_.bind(ep);
    acceptor_.listen();
    bpt::common::log::info("[OkxMdServer] Listening on port {}", port_);
    do_accept();
    thread_ = std::thread([this] { ioc_.run(); });
}

void OkxMdServer::stop() {
    net::post(ioc_, [this] { acceptor_.close(); });
    ioc_.stop();
    if (thread_.joinable())
        thread_.join();
}

void OkxMdServer::do_accept() {
    acceptor_.async_accept([this](beast::error_code ec, tcp::socket socket) {
        if (!ec) {
            bpt::common::log::info("[OkxMdServer] New connection");
            auto session = std::make_shared<OkxMdSession>(std::move(socket));
            sessions_.erase(
                std::remove_if(sessions_.begin(), sessions_.end(), [](const auto& s) { return s->closed(); }),
                sessions_.end());
            sessions_.push_back(session);
            session->run();
        }
        if (acceptor_.is_open())
            do_accept();
    });
}

std::size_t OkxMdServer::session_count() {
    std::promise<std::size_t> p;
    auto fut = p.get_future();
    net::post(ioc_, [this, &p]() {
        auto count = std::count_if(sessions_.begin(), sessions_.end(), [](const auto& s) { return !s->closed(); });
        p.set_value(static_cast<std::size_t>(count));
    });
    return fut.get();
}

void OkxMdServer::push(const data::MarketEvent& event) {
    net::post(ioc_, [this, event]() {
        sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(), [](const auto& s) { return s->closed(); }),
                        sessions_.end());
        if (sessions_.empty())
            return;

        if (event.type == data::MarketEvent::Type::TRADE) {
            const auto& t = std::get<data::TradeRecord>(event.payload);
            auto msg = std::make_shared<std::string>(format_trade(t));
            for (const auto& s : sessions_)
                if (s->is_subscribed("trades", t.symbol))
                    s->send(msg);

        } else {
            const auto& ob = std::get<data::OrderBookRecord>(event.payload);
            auto books5_msg = std::make_shared<std::string>(format_books5(ob));
            auto bbo_tbt_msg = std::make_shared<std::string>(format_bbo_tbt(ob));
            for (const auto& s : sessions_) {
                if (s->is_subscribed("books5", ob.symbol))
                    s->send(books5_msg);
                else if (s->is_subscribed("bbo-tbt", ob.symbol))
                    s->send(bbo_tbt_msg);
            }
        }
    });
}

}  // namespace bpt::backtester::exchange
