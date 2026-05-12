#include "backtester/exchange/hyperliquid_md_server.h"

#include "backtester/data/orderbook_record.h"
#include "backtester/data/trade_record.h"

#include <algorithm>
#include <boost/asio/post.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/json.hpp>
#include <bpt_common/logging.h>
#include <deque>
#include <format>
#include <future>
#include <set>
#include <string>

namespace beast = boost::beast;
namespace ws = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace bpt::backtester::exchange {

namespace {

std::string dbl(double v) {
    return std::format("{:.10g}", v);
}

// HL subscription key: "<type>|<coin>", e.g. "l2Book|BTC".
std::string sub_key(const std::string& type, const std::string& coin) {
    return type + "|" + coin;
}

std::string format_trade(const data::TradeRecord& t) {
    namespace json = boost::json;

    json::object item;
    item["coin"] = t.symbol;
    item["px"] = dbl(t.price);
    item["sz"] = dbl(t.quantity);
    item["side"] = (t.side == data::TradeSide::BUY) ? "B" : "A";
    item["time"] = static_cast<int64_t>(t.timestamp_ns / 1'000'000);

    json::object root;
    root["channel"] = "trades";
    root["data"] = json::array{std::move(item)};
    return json::serialize(root);
}

// HL l2Book wire format: data.levels = [[bids], [asks]] where each level is
// {"px": "...", "sz": "...", "n": N}. We don't track n (order count) in
// backtest, so emit n=1 for each level.
std::string format_l2book(const data::OrderBookRecord& ob) {
    namespace json = boost::json;

    auto build_levels = [](const std::array<double, data::kOrderBookDepth>& px,
                           const std::array<double, data::kOrderBookDepth>& sz) {
        json::array levels;
        for (int i = 0; i < data::kOrderBookDepth; ++i) {
            if (sz[i] <= 0.0)
                continue;  // skip empty levels
            json::object lv;
            lv["px"] = dbl(px[i]);
            lv["sz"] = dbl(sz[i]);
            lv["n"] = 1;
            levels.push_back(std::move(lv));
        }
        return levels;
    };

    json::object data;
    data["coin"] = ob.symbol;
    data["time"] = static_cast<int64_t>(ob.timestamp_ns / 1'000'000);
    data["levels"] = json::array{build_levels(ob.bid_px, ob.bid_sz), build_levels(ob.ask_px, ob.ask_sz)};

    json::object root;
    root["channel"] = "l2Book";
    root["data"] = std::move(data);
    return json::serialize(root);
}

}  // namespace

// ── HyperliquidMdSession ──────────────────────────────────────────────────────

class HyperliquidMdSession : public std::enable_shared_from_this<HyperliquidMdSession> {
public:
    explicit HyperliquidMdSession(tcp::socket socket) : ws_(std::move(socket)) {}

    void run() {
        ws_.async_accept([self = shared_from_this()](beast::error_code ec) { self->on_accept(ec); });
    }

    void send(std::shared_ptr<std::string> msg) {
        bool idle = write_queue_.empty();
        write_queue_.push_back(std::move(msg));
        if (idle)
            do_write();
    }

    bool is_subscribed(const std::string& type, const std::string& coin) const {
        return subs_.count(sub_key(type, coin)) > 0;
    }

    bool closed() const { return closed_; }

private:
    void on_accept(beast::error_code ec) {
        if (ec) {
            bpt::common::log::warn("[HyperliquidMdServer] accept error: {}", ec.message());
            closed_ = true;
            return;
        }
        do_read();
    }

    void do_read() {
        ws_.async_read(buf_, [self = shared_from_this()](beast::error_code ec, std::size_t) { self->on_read(ec); });
    }

    // HL subscribe wire format:
    //   {"method":"subscribe","subscription":{"type":"l2Book","coin":"BTC"}}
    //   {"method":"subscribe","subscription":{"type":"trades","coin":"BTC"}}
    //   {"method":"subscribe","subscription":{"type":"activeAssetCtx","coin":"BTC"}}
    //   {"method":"ping"}  → accepted, replied to with {"channel":"pong"}
    void on_read(beast::error_code ec) {
        if (ec) {
            closed_ = true;
            return;
        }

        std::string text = beast::buffers_to_string(buf_.data());
        buf_.consume(buf_.size());

        try {
            auto val = boost::json::parse(text);
            const auto& obj = val.as_object();
            std::string method = std::string(obj.at("method").as_string());

            if (method == "ping") {
                boost::json::object pong;
                pong["channel"] = "pong";
                send(std::make_shared<std::string>(boost::json::serialize(pong)));
            } else if (method == "subscribe" || method == "unsubscribe") {
                const auto& subscription = obj.at("subscription").as_object();
                std::string type = std::string(subscription.at("type").as_string());
                // "coin" only applies to l2Book / trades / activeAssetCtx; some
                // sub types (e.g. "allMids") have no coin. Default to "" so
                // sub_key still produces something deterministic.
                std::string coin;
                if (auto it = subscription.find("coin"); it != subscription.end())
                    coin = std::string(it->value().as_string());
                std::string key = sub_key(type, coin);

                if (method == "subscribe") {
                    subs_.insert(key);
                    bpt::common::log::info("[HyperliquidMdServer] subscribed: {} {}", type, coin);
                } else {
                    subs_.erase(key);
                }

                // Confirm subscription — HL sends {"channel":"subscriptionResponse",
                // "data":{"method":"subscribe","subscription":{...}}}.
                boost::json::object ack;
                ack["channel"] = "subscriptionResponse";
                boost::json::object ack_data;
                ack_data["method"] = method;
                ack_data["subscription"] = subscription;
                ack["data"] = std::move(ack_data);
                send(std::make_shared<std::string>(boost::json::serialize(ack)));
            }
        } catch (const std::exception& e) {
            bpt::common::log::warn("[HyperliquidMdServer] parse error: {}", e.what());
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

// ── HyperliquidMdServer ───────────────────────────────────────────────────────

HyperliquidMdServer::HyperliquidMdServer(uint16_t port) : port_(port), acceptor_(ioc_) {}

HyperliquidMdServer::~HyperliquidMdServer() {
    stop();
}

void HyperliquidMdServer::start() {
    tcp::endpoint ep{tcp::v4(), port_};
    acceptor_.open(ep.protocol());
    acceptor_.set_option(net::socket_base::reuse_address(true));
    acceptor_.bind(ep);
    acceptor_.listen();
    bpt::common::log::info("[HyperliquidMdServer] Listening on port {}", port_);
    do_accept();
    thread_ = std::thread([this] { ioc_.run(); });
}

void HyperliquidMdServer::stop() {
    net::post(ioc_, [this] { acceptor_.close(); });
    ioc_.stop();
    if (thread_.joinable())
        thread_.join();
}

void HyperliquidMdServer::do_accept() {
    acceptor_.async_accept([this](beast::error_code ec, tcp::socket socket) {
        if (!ec) {
            bpt::common::log::info("[HyperliquidMdServer] New connection");
            auto session = std::make_shared<HyperliquidMdSession>(std::move(socket));
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

std::size_t HyperliquidMdServer::session_count() {
    std::promise<std::size_t> p;
    auto fut = p.get_future();
    net::post(ioc_, [this, &p]() {
        auto count = std::count_if(sessions_.begin(), sessions_.end(), [](const auto& s) { return !s->closed(); });
        p.set_value(static_cast<std::size_t>(count));
    });
    return fut.get();
}

void HyperliquidMdServer::push(const data::MarketEvent& event) {
    net::post(ioc_, [this, event]() {
        sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(), [](const auto& s) { return s->closed(); }),
                        sessions_.end());
        if (sessions_.empty())
            return;

        if (event.type == data::MarketEvent::Type::TRADE) {
            const auto& t = std::get<data::TradeRecord>(event.payload);
            auto msg = std::make_shared<std::string>(format_trade(t));
            int matched = 0;
            for (const auto& s : sessions_) {
                if (s->is_subscribed("trades", t.symbol)) {
                    s->send(msg);
                    ++matched;
                }
            }
            static thread_local int t_count = 0;
            if (++t_count <= 3 || t_count % 1000 == 0)
                bpt::common::log::info("[HyperliquidMdServer] push trade {} matched={}/{}",
                                       t.symbol,
                                       matched,
                                       sessions_.size());
        } else {
            const auto& ob = std::get<data::OrderBookRecord>(event.payload);
            auto msg = std::make_shared<std::string>(format_l2book(ob));
            int matched = 0;
            for (const auto& s : sessions_) {
                if (s->is_subscribed("l2Book", ob.symbol)) {
                    s->send(msg);
                    ++matched;
                }
            }
            static thread_local int b_count = 0;
            if (++b_count <= 3 || b_count % 1000 == 0)
                bpt::common::log::info("[HyperliquidMdServer] push l2Book {} matched={}/{}",
                                       ob.symbol,
                                       matched,
                                       sessions_.size());
        }
    });
}

}  // namespace bpt::backtester::exchange
