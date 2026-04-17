#include "order_gateway/adapter/binance/binance_https_client.h"

#include <boost/beast/http.hpp>

namespace bpt::order_gateway::adapter::binance {

namespace http = boost::beast::http;

BinanceHttpsClient::BinanceHttpsClient(const config::AdapterConfig& cfg,
                                        const ExchangeCredentials& creds)
    : cfg_(cfg),
      api_key_(creds.api_key),
      secret_key_(creds.secret_key),
      inner_(cfg.rest_host, cfg.rest_port) {}

std::string BinanceHttpsClient::request(const std::string& method,
                                         const std::string& path,
                                         const std::string& body,
                                         bool with_api_key) {
    http::verb verb;
    if (method == "POST")
        verb = http::verb::post;
    else if (method == "PUT")
        verb = http::verb::put;
    else if (method == "DELETE")
        verb = http::verb::delete_;
    else
        verb = http::verb::get;

    http::request<http::string_body> req(verb, path, 11);
    req.set(http::field::host, cfg_.rest_host);
    req.set(http::field::user_agent, "bpt-order-gateway/0.1");
    req.set(http::field::content_type, "application/x-www-form-urlencoded");
    if (with_api_key)
        req.set("X-MBX-APIKEY", api_key_);
    req.body() = body;
    req.prepare_payload();

    return inner_.send(req);
}

}  // namespace bpt::order_gateway::adapter::binance
