#include "order_gateway/adapter/okx/okx_https_client.h"

#include "order_gateway/adapter/okx/okx_auth.h"

#include <boost/beast/http.hpp>

namespace bpt::order_gateway::adapter::okx {

namespace http = boost::beast::http;

OKXHttpsClient::OKXHttpsClient(const config::AdapterConfig& cfg, const ExchangeCredentials& creds)
    : cfg_(cfg),
      api_key_(creds.api_key),
      secret_key_(creds.secret_key),
      passphrase_(creds.passphrase),
      inner_(cfg.rest_host, cfg.rest_port) {}

std::string OKXHttpsClient::get_unsigned(const std::string& path) {
    http::request<http::string_body> req(http::verb::get, path, 11);
    req.set(http::field::host, cfg_.rest_host);
    req.set(http::field::user_agent, "bpt-order-gateway/0.1");
    if (cfg_.testnet)
        req.set("x-simulated-trading", "1");
    return inner_.send(req);
}

std::string OKXHttpsClient::get_signed(const std::string& path) {
    http::request<http::string_body> req(http::verb::get, path, 11);
    sign_get_request(req, cfg_.rest_host, path, api_key_, secret_key_, passphrase_, cfg_.testnet);
    return inner_.send(req);
}

}  // namespace bpt::order_gateway::adapter::okx
