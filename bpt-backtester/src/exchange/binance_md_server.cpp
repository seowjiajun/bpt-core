#include "backtester/exchange/binance_md_server.h"

#include "backtester/data/orderbook_record.h"
#include "backtester/data/trade_record.h"

#include <algorithm>
#include <boost/asio/post.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/json.hpp>
#include <deque>
#include <format>
#include <set>
#include <string>
#include <bpt_common/logging.h>

namespace beast = boost::beast;
namespace ws = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace bpt::backtester::exchange {

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string to_lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Formats a double as a decimal string with enough precision for round-trip
// fidelity (up to 10 significant digits, no trailing zeros).
static std::string dbl(double v) {
    return std::format("{:.10g}", v);
}

static std::string format_trade(const data::TradeRecord& t, uint64_t trade_id) {
    namespace json = boost::json;
    std::string sym_low = to_lower(t.symbol);

    json::object data;
    data["e"] = "trade";
    data["E"] = static_cast<int64_t>(t.timestamp_ns / 1'000'000);
    data["s"] = t.symbol;
    data["t"] = static_cast<int64_t>(trade_id);
    data["p"] = dbl(t.price);
    data["q"] = dbl(t.quantity);
    data["T"] = static_cast<int64_t>(t.timestamp_ns / 1'000'000);
    // m=true means buyer is market maker (i.e. taker was the seller → SELL side)
    data["m"] = (t.side == data::TradeSide::SELL);

    json::object root;
    root["stream"] = sym_low + "@trade";
    root["data"] = std::move(data);
    return json::serialize(root);
}

static std::string format_depth(const data::OrderBookRecord& ob, uint64_t update_id, const std::string& stream_name) {
    namespace json = boost::json;

    json::array bids, asks;
    for (int i = 0; i < data::kOrderBookDepth; ++i) {
        bids.push_back(json::array{dbl(ob.bid_px[i]), dbl(ob.bid_sz[i])});
        asks.push_back(json::array{dbl(ob.ask_px[i]), dbl(ob.ask_sz[i])});
    }

    json::object data;
    data["lastUpdateId"] = static_cast<int64_t>(update_id);
    data["bids"] = std::move(bids);
    data["asks"] = std::move(asks);

    json::object root;
    root["stream"] = stream_name;
    root["data"] = std::move(data);
    return json::serialize(root);
}

// ── BinanceSession ────────────────────────────────────────────────────────────

class BinanceSession : public std::enable_shared_from_this<BinanceSession> {
public:
    explicit BinanceSession(tcp::socket socket) : ws_(std::move(socket)) {}

    void run() {
        ws_.async_accept([self = shared_from_this()](beast::error_code ec) { self->on_accept(ec); });
    }

    // Queue a message for async send.  All calls happen on the io_context thread.
    void send(std::shared_ptr<std::string> msg) {
        bool idle = write_queue_.empty();
        write_queue_.push_back(std::move(msg));
        if (idle)
            do_write();
    }

    // Returns the exact subscription string for a depth stream matching
    // sym_lower (e.g. "btcusdt@depth5@100ms"), or "" if not subscribed.
    std::string depth_stream_for(const std::string& sym_lower) const {
        const std::string prefix = sym_lower + "@depth";
        for (const auto& s : subs_)
            if (s.compare(0, prefix.size(), prefix) == 0)
                return s;
        return {};
    }

    bool is_subscribed_trade(const std::string& sym_lower) const { return subs_.count(sym_lower + "@trade") > 0; }

    bool closed() const { return closed_; }

private:
    void on_accept(beast::error_code ec) {
        if (ec) {
            bpt::common::log::warn("[BinanceMdServer] accept error: {}", ec.message());
            closed_ = true;
            return;
        }
        do_read();
    }

    void do_read() {
        ws_.async_read(buf_, [self = shared_from_this()](beast::error_code ec, std::size_t) { self->on_read(ec); });
    }

    void on_read(beast::error_code ec) {
        if (ec) {
            closed_ = true;
            return;
        }

        std::string text = beast::buffers_to_string(buf_.data());
        buf_.consume(buf_.size());

        try {
            auto val = boost::json::parse(text);
            auto& obj = val.as_object();
            std::string method = std::string(obj["method"].as_string());
            int64_t id = obj["id"].to_number<int64_t>();

            if (method == "SUBSCRIBE") {
                for (const auto& p : obj["params"].as_array())
                    subs_.insert(std::string(p.as_string()));
                bpt::common::log::debug("[BinanceMdServer] subscribed: {} streams", subs_.size());
            } else if (method == "UNSUBSCRIBE") {
                for (const auto& p : obj["params"].as_array())
                    subs_.erase(std::string(p.as_string()));
            }

            boost::json::object ack;
            ack["result"] = nullptr;
            ack["id"] = id;
            send(std::make_shared<std::string>(boost::json::serialize(ack)));
        } catch (const std::exception& e) {
            bpt::common::log::warn("[BinanceMdServer] parse error: {}", e.what());
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

// ── BinanceMdServer ───────────────────────────────────────────────────────────

BinanceMdServer::BinanceMdServer(uint16_t port) : port_(port), acceptor_(ioc_) {}

BinanceMdServer::~BinanceMdServer() {
    stop();
}

void BinanceMdServer::start() {
    tcp::endpoint ep{tcp::v4(), port_};
    acceptor_.open(ep.protocol());
    acceptor_.set_option(net::socket_base::reuse_address(true));
    acceptor_.bind(ep);
    acceptor_.listen();
    bpt::common::log::info("[BinanceMdServer] Listening on port {}", port_);
    do_accept();
    thread_ = std::thread([this] { ioc_.run(); });
}

void BinanceMdServer::stop() {
    net::post(ioc_, [this] { acceptor_.close(); });
    ioc_.stop();
    if (thread_.joinable())
        thread_.join();
}

void BinanceMdServer::do_accept() {
    acceptor_.async_accept([this](beast::error_code ec, tcp::socket socket) {
        if (!ec) {
            bpt::common::log::info("[BinanceMdServer] New connection");
            auto session = std::make_shared<BinanceSession>(std::move(socket));
            // Prune stale sessions before adding the new one.
            sessions_.erase(
                std::remove_if(sessions_.begin(), sessions_.end(), [](const auto& s) { return s->closed(); }),
                sessions_.end());
            sessions_.push_back(session);
            session->run();
        }
        if (!ec || ec == beast::errc::operation_canceled) {
            // Only re-arm if acceptor is still open.
            if (acceptor_.is_open())
                do_accept();
        }
    });
}

void BinanceMdServer::push(const data::MarketEvent& event) {
    // Capture by value so the lambda owns the event data.
    net::post(ioc_, [this, event]() {
        // Prune closed sessions.
        sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(), [](const auto& s) { return s->closed(); }),
                        sessions_.end());

        if (sessions_.empty())
            return;

        if (event.type == data::MarketEvent::Type::TRADE) {
            const auto& t = std::get<data::TradeRecord>(event.payload);
            std::string sym = to_lower(t.symbol);
            uint64_t id = ++trade_id_;

            auto msg = std::make_shared<std::string>(format_trade(t, id));
            for (const auto& s : sessions_)
                if (s->is_subscribed_trade(sym))
                    s->send(msg);

        } else {
            const auto& ob = std::get<data::OrderBookRecord>(event.payload);
            std::string sym = to_lower(ob.symbol);
            uint64_t id = ++update_id_;

            // Each session may have subscribed with a different depth stream
            // name (e.g. @depth5 vs @depth5@100ms).  Use their exact name so
            // stream routing in MdGateway's adapter works correctly.
            for (const auto& s : sessions_) {
                std::string stream = s->depth_stream_for(sym);
                if (!stream.empty()) {
                    auto msg = std::make_shared<std::string>(format_depth(ob, id, stream));
                    s->send(std::move(msg));
                }
            }
        }
    });
}

}  // namespace bpt::backtester::exchange
