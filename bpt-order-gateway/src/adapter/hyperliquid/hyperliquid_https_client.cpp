#include "order_gateway/adapter/hyperliquid/hyperliquid_https_client.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <bpt_common/logging.h>
#include <bpt_common/util/tsc_clock.h>
#include <stdexcept>

namespace bpt::order_gateway::adapter::hyperliquid {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

HyperliquidHttpsClient::HyperliquidHttpsClient(std::string host, std::string port, bool use_tls)
    : host_(std::move(host)),
      port_(std::move(port)),
      use_tls_(use_tls) {}

void HyperliquidHttpsClient::connect() {
    tcp::resolver resolver(ioc_);
    const auto results = resolver.resolve(host_, port_);

    if (!use_tls_) {
        plain_stream_ = std::make_unique<beast::tcp_stream>(ioc_);
        plain_stream_->connect(results);
        bpt::common::log::info("HyperliquidHttpsClient: plain TCP connected to {}:{}", host_, port_);
        return;
    }

    // Lazily initialise the ssl::context on first connect so ctor stays cheap.
    static bool ssl_ctx_ready = false;
    if (!ssl_ctx_ready) {
        ssl_ctx_.set_default_verify_paths();
        ssl_ctx_.set_verify_mode(ssl::verify_peer);
        ssl_ctx_ready = true;
    }

    stream_ = std::make_unique<beast::ssl_stream<beast::tcp_stream>>(ioc_, ssl_ctx_);

    if (!SSL_set_tlsext_host_name(stream_->native_handle(), host_.c_str())) {
        stream_.reset();
        throw std::runtime_error("SSL_set_tlsext_host_name failed");
    }

    beast::get_lowest_layer(*stream_).connect(results);
    stream_->handshake(ssl::stream_base::client);

    bpt::common::log::info("HyperliquidHttpsClient: TLS connected to {}:{}", host_, port_);
}

void HyperliquidHttpsClient::close() noexcept {
    if (stream_) {
        beast::error_code ec;
        stream_->shutdown(ec);
        stream_.reset();
    }
    if (plain_stream_) {
        beast::error_code ec;
        plain_stream_->socket().shutdown(tcp::socket::shutdown_both, ec);
        plain_stream_.reset();
    }
}

std::string HyperliquidHttpsClient::post(const std::string& path, const std::string& body) {
    std::lock_guard<std::mutex> lock(mutex_);

    http::request<http::string_body> req(http::verb::post, path, 11);
    req.set(http::field::host, host_);
    req.set(http::field::user_agent, "bpt-order-gateway/0.1");
    req.set(http::field::content_type, "application/json");
    req.keep_alive(true);
    req.body() = body;
    req.prepare_payload();

    // One retry on I/O error: HL closes idle keep-alive connections after
    // ~60 s, so the first send on a stale connection throws. Reconnect
    // once and resend before giving up.
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            const uint64_t t0 = bpt::common::util::TscClock::now_epoch_ns();

            const bool needed_connect = !stream_ && !plain_stream_;
            if (needed_connect)
                connect();
            const uint64_t t_conn = bpt::common::util::TscClock::now_epoch_ns();

            beast::flat_buffer buf;
            http::response<http::string_body> res;
            if (stream_) {
                http::write(*stream_, req);
                http::read(*stream_, buf, res);
            } else {
                http::write(*plain_stream_, req);
                http::read(*plain_stream_, buf, res);
            }
            const uint64_t t_write = t_conn;  // detail timing not split per-stream
            const uint64_t t_read = bpt::common::util::TscClock::now_epoch_ns();

            if (!res.keep_alive())
                close();

            // Per-request timing at DEBUG — enable to trace latency regressions.
            // `server+read` dominates on HL because each /exchange call waits
            // for block inclusion on HL's L1 (~500 ms blocks). TLS pooling
            // eliminates connect cost but HL's commit latency is upstream of
            // us. `/info` queries don't hit the chain so they're always fast.
            bpt::common::log::debug(
                "HyperliquidHttpsClient: post {} "
                "total={:.1f}ms connect={:.1f}ms write={:.2f}ms server+read={:.1f}ms reused={}",
                path,
                (t_read - t0) / 1e6,
                (t_conn - t0) / 1e6,
                (t_write - t_conn) / 1e6,
                (t_read - t_write) / 1e6,
                !needed_connect);

            return res.body();
        } catch (const std::exception& e) {
            bpt::common::log::warn("HyperliquidHttpsClient: post attempt {} failed: {}", attempt, e.what());
            close();
            if (attempt == 1)
                throw;
        }
    }

    // Unreachable — loop either returns on success or throws on retry exhaustion.
    throw std::runtime_error("HyperliquidHttpsClient::post: unreachable");
}

}  // namespace bpt::order_gateway::adapter::hyperliquid
