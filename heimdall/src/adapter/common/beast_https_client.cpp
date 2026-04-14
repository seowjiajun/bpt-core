#include "heimdall/adapter/common/beast_https_client.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <stdexcept>
#include <utility>

namespace heimdall::adapter::common {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

BeastHttpsClient::BeastHttpsClient(std::string host, std::string port)
    : host_(std::move(host)), port_(std::move(port)) {}

std::string BeastHttpsClient::send(http::request<http::string_body>& req) const {
    net::io_context ioc;
    ssl::context ssl_ctx(ssl::context::tls_client);
    ssl_ctx.set_default_verify_paths();
    ssl_ctx.set_verify_mode(ssl::verify_peer);

    tcp::resolver resolver(ioc);
    beast::ssl_stream<beast::tcp_stream> stream(ioc, ssl_ctx);
    if (!SSL_set_tlsext_host_name(stream.native_handle(), host_.c_str()))
        throw std::runtime_error("SSL_set_tlsext_host_name failed");

    auto results = resolver.resolve(host_, port_);
    beast::get_lowest_layer(stream).connect(results);
    stream.handshake(ssl::stream_base::client);

    http::write(stream, req);

    beast::flat_buffer buf;
    http::response<http::string_body> res;
    http::read(stream, buf, res);

    beast::error_code ec;
    stream.shutdown(ec);  // best-effort — ignored

    return res.body();
}

}  // namespace heimdall::adapter::common
