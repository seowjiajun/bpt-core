#include "backtester/exchange/hyperliquid/hyperliquid_info_server.h"

#include <algorithm>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>
#include <bpt_common/logging.h>
#include <fstream>
#include <sstream>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace bpt::backtester::exchange {

namespace {

std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open())
        return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Default HL fee schedule (tier-0 maker/taker). Returned for userFees
// queries since backtest doesn't model per-user volume tiers.
std::string default_fees_response() {
    namespace json = boost::json;
    json::object sched;
    sched["maker"] = "0.00015";
    sched["taker"] = "0.00045";
    sched["referralDiscount"] = "0";
    sched["dailyUsdVolume"] = "0";
    json::object root;
    root["feeSchedule"] = std::move(sched);
    return json::serialize(root);
}

// Synthesised clearinghouseState — what the order-gateway HL adapter parses
// for AccountSnapshot. Static for now: returns starting_capital as both
// accountValue and withdrawable, no positions. Once we wire in MatchingEngine
// state, accountValue should reflect realized PnL and assetPositions should
// list current open positions.
std::string clearinghouse_state_response(double starting_capital) {
    namespace json = boost::json;
    json::object margin;
    margin["accountValue"] = std::format("{:.2f}", starting_capital);
    margin["totalNtlPos"] = "0";
    margin["totalRawUsd"] = std::format("{:.2f}", starting_capital);
    margin["totalMarginUsed"] = "0";

    json::object root;
    root["marginSummary"] = std::move(margin);
    root["crossMarginSummary"] = root.at("marginSummary");
    root["withdrawable"] = std::format("{:.2f}", starting_capital);
    root["crossMaintenanceMarginUsed"] = "0";
    root["assetPositions"] = json::array{};
    root["time"] = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
    return json::serialize(root);
}

}  // namespace

// ── HyperliquidInfoSession ────────────────────────────────────────────────────

class HyperliquidInfoSession : public std::enable_shared_from_this<HyperliquidInfoSession> {
public:
    HyperliquidInfoSession(tcp::socket socket,
                           const std::string& meta_body,
                           const std::string& meta_ctxs_body,
                           const std::string& clearinghouse_body)
        : socket_(std::move(socket)),
          meta_body_(meta_body),
          meta_ctxs_body_(meta_ctxs_body),
          clearinghouse_body_(clearinghouse_body) {}

    void run() { do_read(); }

    bool closed() const { return closed_; }

private:
    void do_read() {
        req_ = {};
        http::async_read(socket_, buf_, req_, [self = shared_from_this()](beast::error_code ec, std::size_t) {
            self->on_read(ec);
        });
    }

    void on_read(beast::error_code ec) {
        if (ec) {
            closed_ = true;
            return;
        }

        // res_ is a SESSION MEMBER (not a local) — Boost.Beast async_write
        // reads the response by reference; if it lived on the on_read stack,
        // it'd be destroyed before async_write completes → UAF segfault on
        // subsequent connections. Same reason req_ is a member.
        res_ = {};
        res_.version(req_.version());
        res_.set(http::field::content_type, "application/json");
        res_.set(http::field::server, "bpt-backtester-info");
        res_.keep_alive(false);
        res_.result(http::status::ok);

        std::string target(req_.target());
        if (req_.method() != http::verb::post || target != "/info") {
            res_.result(http::status::not_found);
            res_.body() = R"({"error":"only POST /info is supported"})";
        } else {
            namespace json = boost::json;
            std::string type;
            try {
                auto parsed = json::parse(std::string(req_.body()));
                if (auto* obj = parsed.if_object()) {
                    if (auto it = obj->find("type"); it != obj->end() && it->value().is_string())
                        type = std::string(it->value().as_string());
                }
            } catch (const std::exception&) {
                // fall through to "unknown type" path
            }

            if (type == "meta") {
                if (meta_body_.empty()) {
                    res_.result(http::status::service_unavailable);
                    res_.body() = R"({"error":"snapshot not loaded"})";
                } else {
                    res_.body() = meta_body_;
                }
            } else if (type == "metaAndAssetCtxs") {
                if (meta_ctxs_body_.empty()) {
                    res_.result(http::status::service_unavailable);
                    res_.body() = R"({"error":"snapshot not loaded"})";
                } else {
                    res_.body() = meta_ctxs_body_;
                }
            } else if (type == "userFees") {
                res_.body() = default_fees_response();
            } else if (type == "clearinghouseState" || type == "spotClearinghouseState") {
                // Both perp and spot clearinghouseState return the same shape.
                // Order-gateway falls back to perp values when spot is empty.
                res_.body() = clearinghouse_body_;
            } else {
                // Unknown info type — return empty object, matching HL's tolerant behaviour.
                res_.body() = "{}";
            }
        }

        res_.prepare_payload();
        http::async_write(socket_, res_, [self = shared_from_this()](beast::error_code ec2, std::size_t) {
            self->closed_ = true;
            if (ec2)
                return;
        });
    }

    tcp::socket socket_;
    beast::flat_buffer buf_;
    http::request<http::string_body> req_;
    http::response<http::string_body> res_;
    const std::string& meta_body_;
    const std::string& meta_ctxs_body_;
    const std::string& clearinghouse_body_;
    bool closed_{false};
};

// ── HyperliquidInfoServer ─────────────────────────────────────────────────────

HyperliquidInfoServer::HyperliquidInfoServer(uint16_t port, std::string snapshot_path, double starting_capital)
    : port_(port),
      snapshot_path_(std::move(snapshot_path)),
      starting_capital_(starting_capital),
      acceptor_(ioc_) {
    load_snapshot();
    clearinghouse_response_body_ = clearinghouse_state_response(starting_capital_);
}

HyperliquidInfoServer::~HyperliquidInfoServer() {
    stop();
}

void HyperliquidInfoServer::load_snapshot() {
    if (snapshot_path_.empty()) {
        bpt::common::log::warn("[HyperliquidInfoServer] no snapshot path configured");
        return;
    }
    const std::string body = read_file(snapshot_path_);
    if (body.empty()) {
        bpt::common::log::warn("[HyperliquidInfoServer] snapshot {} missing or empty", snapshot_path_);
        return;
    }

    namespace json = boost::json;
    boost::json::value parsed;
    try {
        parsed = json::parse(body);
    } catch (const std::exception& e) {
        bpt::common::log::warn("[HyperliquidInfoServer] snapshot parse failed: {}", e.what());
        return;
    }

    // The snapshot can be either:
    //   (a) the raw {"type":"meta"} response: {"universe":[...]}
    //   (b) the raw {"type":"metaAndAssetCtxs"} response: [{"universe":[...]}, [{...}, ...]]
    // Detect by shape and split.
    if (parsed.is_object()) {
        meta_response_body_ = body;
        meta_ctxs_response_body_ = body;  // serve same body if we only have meta
    } else if (parsed.is_array() && parsed.as_array().size() >= 1) {
        meta_response_body_ = json::serialize(parsed.as_array()[0]);
        meta_ctxs_response_body_ = body;
    } else {
        bpt::common::log::warn("[HyperliquidInfoServer] unexpected snapshot shape");
        return;
    }

    // Populate asset_universe_ from the meta response so BacktesterService can
    // wire it into HyperliquidOrderServer.
    try {
        auto meta = json::parse(meta_response_body_);
        if (auto* obj = meta.if_object()) {
            if (auto it = obj->find("universe"); it != obj->end() && it->value().is_array()) {
                for (const auto& asset : it->value().as_array()) {
                    if (auto* a = asset.if_object()) {
                        if (auto n = a->find("name"); n != a->end() && n->value().is_string())
                            asset_universe_.emplace_back(n->value().as_string());
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        bpt::common::log::warn("[HyperliquidInfoServer] universe parse failed: {}", e.what());
    }

    bpt::common::log::info("[HyperliquidInfoServer] loaded snapshot: {} bytes, {} assets",
                           body.size(),
                           asset_universe_.size());
}

void HyperliquidInfoServer::start() {
    tcp::endpoint ep{tcp::v4(), port_};
    acceptor_.open(ep.protocol());
    acceptor_.set_option(net::socket_base::reuse_address(true));
    acceptor_.bind(ep);
    acceptor_.listen();
    bpt::common::log::info("[HyperliquidInfoServer] Listening on port {}", port_);
    do_accept();
    thread_ = std::thread([this] { ioc_.run(); });
}

void HyperliquidInfoServer::stop() {
    net::post(ioc_, [this] { acceptor_.close(); });
    ioc_.stop();
    if (thread_.joinable())
        thread_.join();
}

void HyperliquidInfoServer::do_accept() {
    acceptor_.async_accept([this](beast::error_code ec, tcp::socket socket) {
        if (!ec) {
            sessions_.erase(
                std::remove_if(sessions_.begin(), sessions_.end(), [](const auto& s) { return s->closed(); }),
                sessions_.end());
            auto session = std::make_shared<HyperliquidInfoSession>(std::move(socket),
                                                                    meta_response_body_,
                                                                    meta_ctxs_response_body_,
                                                                    clearinghouse_response_body_);
            sessions_.push_back(session);
            session->run();
        }
        if (acceptor_.is_open())
            do_accept();
    });
}

}  // namespace bpt::backtester::exchange
