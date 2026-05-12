#include "order_gateway/adapter/okx/okx_auth.h"

#include <boost/beast/http/field.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/json.hpp>
#include <bpt_common/util/openssl_helpers.h>
#include <bpt_common/util/tsc_clock.h>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

namespace bpt::order_gateway::adapter::okx {

namespace http = boost::beast::http;
namespace json = boost::json;

using bpt::common::util::base64_encode;
using bpt::common::util::hmac_sha256;
using bpt::common::util::WallClock;

namespace {

// OKX REST auth requires the timestamp as ISO 8601 UTC with millisecond
// precision, e.g. "2026-04-15T05:54:43.123Z". The same string is used
// in the prehash string and in the OK-ACCESS-TIMESTAMP header. (The WS
// public/auth login uses Unix epoch seconds instead — two different
// formats for the same account, both documented.)
std::string iso8601_now_ms() {
    const auto now = std::chrono::system_clock::now();
    const auto ms_part = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_utc{};
    gmtime_r(&t, &tm_utc);
    char date_buf[32];
    std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%dT%H:%M:%S", &tm_utc);
    char out[48];
    std::snprintf(out, sizeof(out), "%s.%03lldZ", date_buf, static_cast<long long>(ms_part.count()));
    return out;
}

std::string hmac_sha256_b64(std::string_view key, std::string_view data) {
    return base64_encode(hmac_sha256(key, data));
}

}  // namespace

std::string build_login_msg(std::string_view api_key, std::string_view secret_key, std::string_view passphrase) {
    const std::string ts_str = std::to_string(WallClock::now_s());
    const std::string prehash = ts_str + "GET" + "/users/self/verify";
    const std::string sign = hmac_sha256_b64(secret_key, prehash);

    json::object arg;
    arg["apiKey"] = std::string(api_key);
    arg["passphrase"] = std::string(passphrase);
    arg["timestamp"] = ts_str;
    arg["sign"] = sign;

    json::object msg;
    msg["op"] = "login";
    msg["args"] = json::array{std::move(arg)};
    return json::serialize(msg);
}

void sign_get_request(http::request<http::string_body>& req,
                      std::string_view host,
                      std::string_view path,
                      std::string_view api_key,
                      std::string_view secret_key,
                      std::string_view passphrase,
                      bool testnet) {
    const std::string ts_str = iso8601_now_ms();
    const std::string prehash = ts_str + "GET" + std::string(path);
    const std::string sign = hmac_sha256_b64(secret_key, prehash);

    req.set(http::field::host, std::string(host));
    req.set(http::field::user_agent, "bpt-order-gateway/0.1");
    req.set("OK-ACCESS-KEY", std::string(api_key));
    req.set("OK-ACCESS-SIGN", sign);
    req.set("OK-ACCESS-TIMESTAMP", ts_str);
    req.set("OK-ACCESS-PASSPHRASE", std::string(passphrase));
    if (testnet)
        req.set("x-simulated-trading", "1");
}

}  // namespace bpt::order_gateway::adapter::okx
