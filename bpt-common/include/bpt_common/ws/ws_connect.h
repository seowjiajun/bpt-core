#pragma once

// yggdrasil/ws_connect.h — WebSocket connect helpers (Boost.Beast).
//
// Provides TLS (ws_connect) and plain-TCP (ws_connect_plain) variants plus
// AnyWsStream — a type-erased wrapper that exposes a uniform read/write API
// over either stream type.  Adapters that support backtest mode (use_tls=false)
// return AnyWsStream from connect_and_subscribe() and accept it in read_loop().

#include <algorithm>
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <cctype>
#include <chrono>
#include <memory>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace bpt::common::ws {

// ── Stream type aliases ───────────────────────────────────────────────────────

using WsStream = boost::beast::websocket::stream<boost::beast::ssl_stream<boost::beast::tcp_stream>>;
using PlainWsStream = boost::beast::websocket::stream<boost::beast::tcp_stream>;

// ── AnyWsStream ───────────────────────────────────────────────────────────────
//
// Type-erased wrapper over WsStream (TLS) or PlainWsStream (plain TCP).
// Exposes the subset of the Beast websocket stream API used by all adapters:
//   text, set_option, write, read, close, expires_after.
//
// Usage:
//   AnyWsStream ws(ws_connect(...));         // TLS
//   AnyWsStream ws(ws_connect_plain(...));   // plain TCP
class AnyWsStream {
    std::variant<std::unique_ptr<WsStream>, std::unique_ptr<PlainWsStream>> v_;

    template <typename F>
    decltype(auto) visit(F&& f) {
        return std::visit([&f](auto& p) -> decltype(auto) { return f(*p); }, v_);
    }

public:
    explicit AnyWsStream(std::unique_ptr<WsStream> s) : v_(std::move(s)) {}
    explicit AnyWsStream(std::unique_ptr<PlainWsStream> s) : v_(std::move(s)) {}

    void text(bool v) {
        visit([v](auto& ws) { ws.text(v); });
    }

    template <typename Opt>
    void set_option(Opt&& opt) {
        visit([&](auto& ws) { ws.set_option(std::forward<Opt>(opt)); });
    }

    template <typename B>
    void write(const B& buf) {
        visit([&](auto& ws) { ws.write(buf); });
    }

    void read(boost::beast::flat_buffer& buf, boost::beast::error_code& ec) {
        visit([&](auto& ws) { ws.read(buf, ec); });
    }

    // Async read. The Beast version currently vendored ignores
    // expires_after for SYNCHRONOUS ws.read(), which makes the
    // timeout-driven on_tick() in RunLoop unreliable. Switching to
    // async_read + an explicit steady_timer in RunLoop is the proper
    // fix; this overload is the AnyWsStream side of that change.
    //
    // Handler signature is the Beast websocket async_read contract:
    //   void(boost::beast::error_code ec, std::size_t bytes_transferred)
    template <typename Handler>
    void async_read(boost::beast::flat_buffer& buf, Handler&& handler) {
        visit([&](auto& ws) { ws.async_read(buf, std::forward<Handler>(handler)); });
    }

    void close(boost::beast::websocket::close_code code) {
        visit([code](auto& ws) {
            boost::beast::error_code ec;
            ws.close(code, ec);
            // Ignore close errors — best-effort cleanup.
        });
    }

    template <typename Duration>
    void expires_after(Duration d) {
        visit([&d](auto& ws) { boost::beast::get_lowest_layer(ws).expires_after(d); });
    }

    void expires_never() {
        visit([](auto& ws) { boost::beast::get_lowest_layer(ws).expires_never(); });
    }

    // Cancel any pending async operations on the underlying TCP socket.
    // Used by RunLoop to wake the pumping loop out of a pending
    // async_read on shutdown — the read handler sees
    // operation_aborted and returns without re-arming.
    void lowest_layer_cancel(boost::beast::error_code& ec) {
        visit([&ec](auto& ws) {
            boost::beast::get_lowest_layer(ws).socket().cancel(ec);
        });
    }
};

// Lowercase-hex SHA-256 of the leaf cert's DER encoding. Used by the
// TLS-pinning path in ws_connect — matches the output of
//   openssl x509 -fingerprint -sha256 -noout | tr -d :aF-F | tr A-F a-f
// (i.e. 64 hex chars, no colons, lowercase).
inline std::string cert_sha256_hex(X509* cert) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (!X509_digest(cert, EVP_sha256(), md, &len))
        return "";
    static constexpr char digits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(static_cast<std::size_t>(len) * 2);
    for (unsigned int i = 0; i < len; ++i) {
        hex.push_back(digits[md[i] >> 4]);
        hex.push_back(digits[md[i] & 0xf]);
    }
    return hex;
}

// ── ws_connect (TLS) ──────────────────────────────────────────────────────────
//
// Resolve host:port, perform TCP connect, TLS handshake, and WebSocket upgrade.
// Throws on any failure. Intended to be called from connect_and_subscribe().
//
// so_rcvbuf_bytes:    receive buffer size (0 = OS default).
// connect_timeout_ms: hard deadline for the entire connect sequence.
//                     Cleared with expires_never() before returning.
// user_agent:         value sent in the HTTP Upgrade User-Agent header.
// pinned_cert_sha256: optional allowlist of leaf-cert SHA-256 fingerprints
//                     (lowercase hex, no colons, 64 chars each). Empty = no
//                     pinning (CA + hostname verification only, as before).
//                     Non-empty = connection rejected unless the leaf cert's
//                     hash matches one of the pins. Operator must rotate
//                     pins when the exchange rotates its TLS cert.
inline std::unique_ptr<WsStream> ws_connect(boost::asio::io_context& ioc,
                                            boost::asio::ssl::context& ssl_ctx,
                                            const std::string& host,
                                            const std::string& port,
                                            const std::string& path,
                                            uint32_t so_rcvbuf_bytes = 0,
                                            uint32_t connect_timeout_ms = 30000,
                                            const std::string& user_agent = "bpt-client/0.1",
                                            const std::vector<std::string>& pinned_cert_sha256 = {}) {
    boost::asio::ip::tcp::resolver resolver(ioc);
    auto ws = std::make_unique<WsStream>(ioc, ssl_ctx);

    boost::beast::get_lowest_layer(*ws).expires_after(std::chrono::milliseconds(connect_timeout_ms));

    auto results = resolver.resolve(host, port);
    boost::beast::get_lowest_layer(*ws).connect(results);

    boost::beast::get_lowest_layer(*ws).socket().set_option(boost::asio::ip::tcp::no_delay(true));

    if (so_rcvbuf_bytes > 0) {
        boost::beast::get_lowest_layer(*ws).socket().set_option(
            boost::asio::socket_base::receive_buffer_size(static_cast<int>(so_rcvbuf_bytes)));
    }

    if (!SSL_set_tlsext_host_name(ws->next_layer().native_handle(), host.c_str()))
        throw std::runtime_error("SSL_set_tlsext_host_name failed");

    // Compose host-name verification with optional cert pinning. Pin
    // check only runs at the leaf depth (get_error_depth() == 0) so we
    // don't reject on intermediate/root CAs. Missing cert or hash
    // computation failure → rejected.
    if (pinned_cert_sha256.empty()) {
        ws->next_layer().set_verify_callback(boost::asio::ssl::host_name_verification(host));
    } else {
        ws->next_layer().set_verify_callback([host, pins = pinned_cert_sha256](bool preverified,
                                                                               boost::asio::ssl::verify_context& ctx) {
            boost::asio::ssl::host_name_verification hnv(host);
            if (!hnv(preverified, ctx))
                return false;
            auto* store_ctx = ctx.native_handle();
            if (X509_STORE_CTX_get_error_depth(store_ctx) != 0)
                return true;  // only pin the leaf
            X509* cert = X509_STORE_CTX_get_current_cert(store_ctx);
            if (!cert)
                return false;
            const std::string got = cert_sha256_hex(cert);
            if (got.empty())
                return false;
            for (const auto& pin : pins) {
                if (pin.size() != got.size())
                    continue;
                bool match = std::equal(pin.begin(), pin.end(), got.begin(), [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
                });
                if (match)
                    return true;
            }
            return false;
        });
    }
    ws->next_layer().handshake(boost::asio::ssl::stream_base::client);

    ws->set_option(
        boost::beast::websocket::stream_base::decorator([user_agent](boost::beast::websocket::request_type& req) {
            req.set(boost::beast::http::field::user_agent, user_agent);
        }));
    ws->handshake(host, path);

    boost::beast::get_lowest_layer(*ws).expires_never();
    return ws;
}

// ── ws_connect_plain (plain TCP, no TLS) ─────────────────────────────────────
//
// Same as ws_connect but skips TLS — intended for local simulation servers
// (e.g. Backtester mock exchange) where TLS is not needed.
inline std::unique_ptr<PlainWsStream> ws_connect_plain(boost::asio::io_context& ioc,
                                                       const std::string& host,
                                                       const std::string& port,
                                                       const std::string& path,
                                                       uint32_t so_rcvbuf_bytes = 0,
                                                       uint32_t connect_timeout_ms = 30000,
                                                       const std::string& user_agent = "bpt-client/0.1") {
    boost::asio::ip::tcp::resolver resolver(ioc);
    auto ws = std::make_unique<PlainWsStream>(ioc);

    boost::beast::get_lowest_layer(*ws).expires_after(std::chrono::milliseconds(connect_timeout_ms));

    auto results = resolver.resolve(host, port);
    boost::beast::get_lowest_layer(*ws).connect(results);

    boost::beast::get_lowest_layer(*ws).socket().set_option(boost::asio::ip::tcp::no_delay(true));

    if (so_rcvbuf_bytes > 0) {
        boost::beast::get_lowest_layer(*ws).socket().set_option(
            boost::asio::socket_base::receive_buffer_size(static_cast<int>(so_rcvbuf_bytes)));
    }

    ws->set_option(
        boost::beast::websocket::stream_base::decorator([user_agent](boost::beast::websocket::request_type& req) {
            req.set(boost::beast::http::field::user_agent, user_agent);
        }));
    ws->handshake(host, path);

    boost::beast::get_lowest_layer(*ws).expires_never();
    return ws;
}

}  // namespace bpt::common::ws
