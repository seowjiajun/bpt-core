#include "backtester/exchange/hyperliquid_order_server.h"

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

namespace {

std::string dbl(double v) {
    return std::format("{:.10g}", v);
}

// HL userFills item — what the strategy parses to detect fills.
boost::json::object format_fill_item(const matching::FillReport& fill, uint64_t oid) {
    namespace json = boost::json;
    json::object f;
    f["coin"] = fill.symbol;
    f["oid"] = static_cast<int64_t>(oid);
    f["side"] = (fill.side == matching::OrderSide::BUY) ? "B" : "A";
    f["px"] = dbl(fill.last_fill_price);
    f["sz"] = dbl(fill.last_fill_qty);
    // HL maker fee ~0.015%, taker ~0.045% per side; use 0.025% midpoint.
    f["fee"] = dbl(fill.last_fill_qty * fill.last_fill_price * 0.00025);
    f["time"] = static_cast<int64_t>(fill.simulation_ts / 1'000'000);
    f["closedPnl"] = "0";
    f["dir"] = (fill.side == matching::OrderSide::BUY) ? "Open Long" : "Open Short";
    f["startPosition"] = "0";
    f["hash"] = "0x0";
    f["crossed"] = false;
    f["tid"] = static_cast<int64_t>(oid);
    return f;
}

}  // namespace

// ── HyperliquidOrderSession ───────────────────────────────────────────────────

class HyperliquidOrderSession : public std::enable_shared_from_this<HyperliquidOrderSession> {
public:
    HyperliquidOrderSession(tcp::socket socket,
                            matching::MatchingEngine& engine,
                            std::atomic<uint64_t>& order_id_seq,
                            const std::vector<std::string>& asset_universe)
        : ws_(std::move(socket)),
          engine_(engine),
          order_id_seq_(order_id_seq),
          asset_universe_(asset_universe) {}

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
            bpt::common::log::warn("[HyperliquidOrderServer] accept error: {}", ec.message());
            closed_ = true;
            return;
        }
        bpt::common::log::info("[HyperliquidOrderServer] WS connected");
        do_read();
    }

    void do_read() {
        ws_.async_read(buf_, [self = shared_from_this()](beast::error_code ec, std::size_t) { self->on_read(ec); });
    }

    // HL message envelopes:
    //   {"method":"subscribe","subscription":{"type":"userFills","user":"0x..."}}
    //   {"method":"post","id":N,"request":{"type":"action",
    //    "payload":{"action":{...},"nonce":...,"signature":{...}}}}
    //   {"method":"ping"}
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
                // Ack subscriptions (userFills, etc.) so the adapter can
                // start its read loop. No state tracked — fills broadcast
                // to every session.
                boost::json::object ack;
                ack["channel"] = "subscriptionResponse";
                boost::json::object data;
                data["method"] = method;
                data["subscription"] = obj.at("subscription");
                ack["data"] = std::move(data);
                send(std::make_shared<std::string>(boost::json::serialize(ack)));
            } else if (method == "post") {
                handle_post(obj);
            }
        } catch (const std::exception& e) {
            bpt::common::log::warn("[HyperliquidOrderServer] parse error: {}", e.what());
        }

        do_read();
    }

    void handle_post(const boost::json::object& obj) {
        namespace json = boost::json;

        int64_t req_id = obj.at("id").to_number<int64_t>();
        const auto& request = obj.at("request").as_object();
        const auto& payload = request.at("payload").as_object();
        const auto& action = payload.at("action").as_object();
        std::string action_type = std::string(action.at("type").as_string());

        // HL WS post payload shape: {"status":"ok","response":{"type":"<action>",
        // "data":{"statuses":[...]}}}. The order-gateway's HyperliquidExecEmitter
        // reads `status` at the top level and bails to REJECTED if it's not
        // "ok"; previous nesting put `type:ok` under payload directly, which
        // emit_order_response doesn't recognise → every order falsely rejected.
        json::object inner_response;
        if (action_type == "order") {
            inner_response["type"] = "order";
            inner_response["data"] = handle_order_action(action);
        } else if (action_type == "cancel") {
            inner_response["type"] = "cancel";
            inner_response["data"] = handle_cancel_action(action);
        } else {
            // Unknown action type — return ok empty so client unblocks.
            inner_response["type"] = action_type;
            inner_response["data"] = json::object{};
        }

        json::object response_payload;
        response_payload["status"] = "ok";
        response_payload["response"] = std::move(inner_response);

        json::object response;
        response["type"] = "action";
        response["payload"] = std::move(response_payload);

        json::object data;
        data["id"] = req_id;
        data["response"] = std::move(response);

        json::object root;
        root["channel"] = "post";
        root["data"] = std::move(data);
        send(std::make_shared<std::string>(json::serialize(root)));
    }

    boost::json::value handle_order_action(const boost::json::object& action) {
        namespace json = boost::json;
        json::array statuses;

        for (const auto& jv : action.at("orders").as_array()) {
            const auto& o = jv.as_object();
            uint64_t asset_idx = o.at("a").to_number<uint64_t>();
            bool is_buy = o.at("b").as_bool();
            std::string px = std::string(o.at("p").as_string());
            std::string sz = std::string(o.at("s").as_string());

            // Asset index → coin via universe lookup; fall back to universe[0]
            // when out of range (single-symbol backtest case).
            std::string coin = (asset_idx < asset_universe_.size())
                                   ? asset_universe_[asset_idx]
                                   : (asset_universe_.empty() ? std::string{"UNKNOWN"} : asset_universe_[0]);

            // Pull tif from t.limit.tif if present.
            std::string tif = "Gtc";
            if (auto t_it = o.find("t"); t_it != o.end() && t_it->value().is_object()) {
                const auto& t_obj = t_it->value().as_object();
                if (auto limit_it = t_obj.find("limit"); limit_it != t_obj.end() && limit_it->value().is_object()) {
                    const auto& limit_obj = limit_it->value().as_object();
                    if (auto tif_it = limit_obj.find("tif"); tif_it != limit_obj.end())
                        tif = std::string(tif_it->value().as_string());
                }
            }

            matching::OpenOrder order;
            order.exchange = "HYPERLIQUID";
            order.symbol = coin;
            // HL doesn't carry a clOrdId in the action; use the order_id we
            // mint here as both the exchange oid and the client tag for
            // matching-engine bookkeeping.
            uint64_t oid = ++order_id_seq_;
            order.client_order_id = std::to_string(oid);
            order.order_id = std::to_string(oid);
            // HL signals POST_ONLY via tif="Alo" (Add Liquidity Only).
            order.type = (tif == "Alo") ? matching::OrderType::POST_ONLY : matching::OrderType::LIMIT;
            order.side = is_buy ? matching::OrderSide::BUY : matching::OrderSide::SELL;
            order.quantity = std::stod(sz);
            order.price = std::stod(px);

            const auto submitted = engine_.submit_order(std::move(order));

            json::object status;
            if (submitted.rejected) {
                // HL rejection format: status carries an "error" string.
                // Specific phrasing matches HL's "Order would immediately
                // match" error so the OGW decoder routes it to a REJECTED
                // ExecReport instead of an ACK.
                status["error"] = "Order would immediately match and was rejected because POST_ONLY (Alo) was set.";
            } else {
                json::object resting;
                resting["oid"] = static_cast<int64_t>(oid);
                status["resting"] = std::move(resting);
            }
            statuses.push_back(std::move(status));
        }

        json::object data;
        data["statuses"] = std::move(statuses);
        return data;
    }

    boost::json::value handle_cancel_action(const boost::json::object& action) {
        namespace json = boost::json;
        json::array statuses;
        for (const auto& jv : action.at("cancels").as_array()) {
            const auto& c = jv.as_object();
            uint64_t asset_idx = c.at("a").to_number<uint64_t>();
            uint64_t oid = c.at("o").to_number<uint64_t>();
            std::string coin = (asset_idx < asset_universe_.size())
                                   ? asset_universe_[asset_idx]
                                   : (asset_universe_.empty() ? std::string{"UNKNOWN"} : asset_universe_[0]);
            bool ok = engine_.cancel_order("HYPERLIQUID", coin, std::to_string(oid));
            statuses.push_back(json::value(ok ? "success" : "Order not found"));
        }
        json::object data;
        data["statuses"] = std::move(statuses);
        return data;
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
    const std::vector<std::string>& asset_universe_;
    bool closed_{false};
};

// ── HyperliquidOrderServer ────────────────────────────────────────────────────

HyperliquidOrderServer::HyperliquidOrderServer(uint16_t port,
                                               matching::MatchingEngine& engine,
                                               std::vector<std::string> asset_universe)
    : port_(port),
      engine_(engine),
      asset_universe_(std::move(asset_universe)),
      acceptor_(ioc_) {}

HyperliquidOrderServer::~HyperliquidOrderServer() {
    stop();
}

void HyperliquidOrderServer::start() {
    tcp::endpoint ep{tcp::v4(), port_};
    acceptor_.open(ep.protocol());
    acceptor_.set_option(net::socket_base::reuse_address(true));
    acceptor_.bind(ep);
    acceptor_.listen();
    bpt::common::log::info("[HyperliquidOrderServer] Listening on port {}", port_);
    do_accept();
    thread_ = std::thread([this] { ioc_.run(); });
}

void HyperliquidOrderServer::stop() {
    net::post(ioc_, [this] { acceptor_.close(); });
    ioc_.stop();
    if (thread_.joinable())
        thread_.join();
}

void HyperliquidOrderServer::do_accept() {
    acceptor_.async_accept([this](beast::error_code ec, tcp::socket socket) {
        if (!ec) {
            sessions_.erase(
                std::remove_if(sessions_.begin(), sessions_.end(), [](const auto& s) { return s->closed(); }),
                sessions_.end());
            auto session =
                std::make_shared<HyperliquidOrderSession>(std::move(socket), engine_, order_id_seq_, asset_universe_);
            sessions_.push_back(session);
            session->run();
        }
        if (acceptor_.is_open())
            do_accept();
    });
}

void HyperliquidOrderServer::push_fill(const matching::FillReport& fill) {
    uint64_t oid = 0;
    try {
        oid = std::stoull(fill.order_id);
    } catch (...) {
    }

    namespace json = boost::json;
    json::object inner;
    inner["isSnapshot"] = false;
    inner["user"] = "0x0000000000000000000000000000000000000000";
    inner["fills"] = json::array{format_fill_item(fill, oid)};

    json::object root;
    root["channel"] = "userFills";
    root["data"] = std::move(inner);

    auto msg = std::make_shared<std::string>(json::serialize(root));

    net::post(ioc_, [this, msg]() {
        sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(), [](const auto& s) { return s->closed(); }),
                        sessions_.end());
        for (const auto& s : sessions_)
            s->send(msg);
    });
}

}  // namespace bpt::backtester::exchange
