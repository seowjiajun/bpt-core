#include "book/adapter/hyperliquid_balance_adapter.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/json.hpp>

#include <chrono>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <bpt_common/logging.h>

// HL account endpoints are public — /info accepts unsigned POST. No
// signer, no credentials. Only the wallet address is sensitive (and
// even that is a public identifier). Lives in its own file — a future
// refactor might lift the Boost.Beast HTTPS bits into bpt-common, but
// not before a second adapter actually reuses it.

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
namespace json = boost::json;
using tcp = net::ip::tcp;

namespace bpt::book::adapter {

HyperliquidBalanceAdapter::HyperliquidBalanceAdapter(Config cfg) : cfg_(std::move(cfg)) {
    if (cfg_.wallet_address.empty())
        throw std::runtime_error("HyperliquidBalanceAdapter: wallet_address is required");
    if (cfg_.rest_host.empty())
        throw std::runtime_error("HyperliquidBalanceAdapter: rest_host is required");
}

std::string HyperliquidBalanceAdapter::post(const std::string& body) const {
    net::io_context ioc;
    ssl::context ssl_ctx(ssl::context::tls_client);
    ssl_ctx.set_default_verify_paths();
    ssl_ctx.set_verify_mode(ssl::verify_peer);

    tcp::resolver resolver(ioc);
    beast::ssl_stream<beast::tcp_stream> stream(ioc, ssl_ctx);
    if (!SSL_set_tlsext_host_name(stream.native_handle(), cfg_.rest_host.c_str()))
        throw std::runtime_error("SSL_set_tlsext_host_name failed");

    auto results = resolver.resolve(cfg_.rest_host, cfg_.rest_port);
    beast::get_lowest_layer(stream).connect(results);
    stream.handshake(ssl::stream_base::client);

    http::request<http::string_body> req{http::verb::post, "/info", 11};
    req.set(http::field::host, cfg_.rest_host);
    req.set(http::field::user_agent, "bpt-book/0.1");
    req.set(http::field::content_type, "application/json");
    req.body() = body;
    req.prepare_payload();

    http::write(stream, req);

    beast::flat_buffer buf;
    http::response<http::string_body> res;
    http::read(stream, buf, res);

    beast::error_code ec;
    stream.shutdown(ec);  // best-effort

    if (res.result() != http::status::ok)
        throw std::runtime_error("HL /info returned status " + std::to_string(res.result_int()));

    return res.body();
}

namespace {

int64_t to_e8(const char* s) {
    if (!s) return 0;
    return static_cast<int64_t>(std::round(std::stod(s) * 1e8));
}
int64_t to_e8(const std::string& s) { return to_e8(s.c_str()); }

}  // namespace

std::vector<BalanceRow> HyperliquidBalanceAdapter::fetch() {
    std::vector<BalanceRow> rows;

    // ── Perps (clearinghouseState) ───────────────────────────────────
    // Returns a single USDC margin account. accountValue = total equity
    // including unrealized PnL; withdrawable = free cash after margin
    // requirements. hold = total - free.
    {
        json::object req;
        req["type"] = "clearinghouseState";
        req["user"] = cfg_.wallet_address;
        const std::string resp = post(json::serialize(json::value(req)));

        auto v = json::parse(resp).as_object();
        int64_t total_e8 = 0;
        int64_t free_e8 = 0;
        if (auto it = v.find("marginSummary"); it != v.end() && it->value().is_object()) {
            const auto& ms = it->value().as_object();
            if (auto a = ms.find("accountValue"); a != ms.end() && a->value().is_string())
                total_e8 = to_e8(std::string(a->value().as_string()));
        }
        if (auto it = v.find("withdrawable"); it != v.end() && it->value().is_string())
            free_e8 = to_e8(std::string(it->value().as_string()));

        // Skip all-zero rows to keep the published snapshot tight.
        if (total_e8 != 0 || free_e8 != 0) {
            BalanceRow r;
            r.exchange_id = bpt::messages::ExchangeId::HYPERLIQUID;
            r.sub_account = "perps";
            r.ccy = "USDC";
            r.total_e8 = total_e8;
            r.free_e8 = free_e8;
            r.hold_e8 = std::max<int64_t>(0, total_e8 - free_e8);
            rows.push_back(std::move(r));
        }
    }

    // ── Spot (spotClearinghouseState) ─────────────────────────────────
    // Returns a list of coins with total + hold. free = total - hold.
    // Multiple currencies possible (USDC, spot tokens, etc.).
    {
        json::object req;
        req["type"] = "spotClearinghouseState";
        req["user"] = cfg_.wallet_address;
        const std::string resp = post(json::serialize(json::value(req)));

        auto v = json::parse(resp).as_object();
        if (auto it = v.find("balances"); it != v.end() && it->value().is_array()) {
            for (const auto& b_val : it->value().as_array()) {
                if (!b_val.is_object()) continue;
                const auto& b = b_val.as_object();

                std::string coin;
                if (auto cit = b.find("coin"); cit != b.end() && cit->value().is_string())
                    coin = std::string(cit->value().as_string());
                if (coin.empty()) continue;

                int64_t total_e8 = 0;
                int64_t hold_e8 = 0;
                if (auto t = b.find("total"); t != b.end() && t->value().is_string())
                    total_e8 = to_e8(std::string(t->value().as_string()));
                if (auto h = b.find("hold"); h != b.end() && h->value().is_string())
                    hold_e8 = to_e8(std::string(h->value().as_string()));
                if (total_e8 == 0 && hold_e8 == 0)
                    continue;

                BalanceRow r;
                r.exchange_id = bpt::messages::ExchangeId::HYPERLIQUID;
                r.sub_account = "spot";
                r.ccy = coin;
                r.total_e8 = total_e8;
                r.hold_e8 = hold_e8;
                r.free_e8 = std::max<int64_t>(0, total_e8 - hold_e8);
                rows.push_back(std::move(r));
            }
        }
    }

    return rows;
}

}  // namespace bpt::book::adapter
