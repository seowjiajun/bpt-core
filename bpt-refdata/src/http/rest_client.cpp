#include "refdata/http/rest_client.h"

#include <array>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <yggdrasil/logging.h>

namespace bpt::refdata::http {

namespace {

namespace beast = boost::beast;
namespace bhttp = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

// Sends req and reads the response body.  Throws on non-200.
template <typename Stream, typename ReqBody>
std::string send_recv(Stream& stream, bhttp::request<ReqBody>& req) {
    bhttp::write(stream, req);
    beast::flat_buffer buf;
    bhttp::response_parser<bhttp::string_body> parser;
    parser.body_limit(32 * 1024 * 1024);  // 32 MiB — exchangeInfo can be large
    bhttp::read(stream, buf, parser);
    auto res = parser.get();
    if (res.result() != bhttp::status::ok)
        throw std::runtime_error("HTTP " + std::to_string(static_cast<int>(res.result())) + " for " +
                                 std::string(req.target()));
    return res.body();
}

template <typename ReqBody>
std::string do_request(const std::string& host,
                       const std::string& port,
                       bool use_tls,
                       ssl::context& ssl_ctx,
                       bhttp::request<ReqBody>& req) {
    net::io_context ioc;
    tcp::resolver resolver(ioc);
    auto results = resolver.resolve(host, port);

    if (use_tls) {
        beast::ssl_stream<beast::tcp_stream> stream(ioc, ssl_ctx);
        if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()))
            throw beast::system_error(beast::errc::make_error_code(beast::errc::not_connected));
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(30));
        beast::get_lowest_layer(stream).connect(results);
        stream.handshake(ssl::stream_base::client);
        return send_recv(stream, req);
    } else {
        beast::tcp_stream stream(ioc);
        stream.expires_after(std::chrono::seconds(30));
        stream.connect(results);
        return send_recv(stream, req);
    }
}

// Retry wrapper: 3 attempts with 1 s / 2 s / 4 s backoff.
// Does NOT retry on HTTP 4xx (client errors — auth failures, bad requests).
template <typename Fn>
std::string with_retry(Fn&& fn, const std::string& desc) {
    static constexpr std::array<int, 3> kBackoffS = {1, 2, 4};
    static constexpr int kMaxAttempts = 3;

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        try {
            return fn();
        } catch (const std::exception& e) {
            const std::string msg = e.what();
            // 4xx = client error; retrying won't help.
            if (msg.find("HTTP 4") != std::string::npos)
                throw;
            if (attempt + 1 == kMaxAttempts)
                throw;
            ygg::log::warn("[RestClient] {} failed (attempt {}/{}): {} — retrying in {}s",
                           desc,
                           attempt + 1,
                           kMaxAttempts,
                           msg,
                           kBackoffS[attempt]);
            std::this_thread::sleep_for(std::chrono::seconds(kBackoffS[attempt]));
        }
    }
    throw std::runtime_error("unreachable");
}

}  // namespace

RestClient::RestClient(std::string host, std::string port, bool use_tls)
    : host_(std::move(host)),
      port_(std::move(port)),
      use_tls_(use_tls),
      ssl_ctx_(ssl::context::tls_client) {
    // Load CA certificates once at construction; reused for all subsequent requests.
    if (use_tls_)
        ssl_ctx_.set_default_verify_paths();
}

std::string RestClient::get(const std::string& target, const Headers& extra_headers) const {
    bhttp::request<bhttp::empty_body> req{bhttp::verb::get, target, 11};
    req.set(bhttp::field::host, host_);
    req.set(bhttp::field::user_agent, "bpt-refdata/1.0");
    for (const auto& [k, v] : extra_headers)
        req.set(k, v);
    return with_retry([&]() { return do_request(host_, port_, use_tls_, ssl_ctx_, req); }, target);
}

std::string RestClient::post(const std::string& target, const std::string& body, const Headers& extra_headers) const {
    bhttp::request<bhttp::string_body> req{bhttp::verb::post, target, 11};
    req.set(bhttp::field::host, host_);
    req.set(bhttp::field::user_agent, "bpt-refdata/1.0");
    req.set(bhttp::field::content_type, "application/json");
    req.body() = body;
    req.prepare_payload();
    for (const auto& [k, v] : extra_headers)
        req.set(k, v);
    return with_retry([&]() { return do_request(host_, port_, use_tls_, ssl_ctx_, req); }, target);
}

}  // namespace bpt::refdata::http
