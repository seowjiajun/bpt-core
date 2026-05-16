#include "refdata/adapter/okx/okx_refdata_auth.h"

#include <bpt_common/util/openssl_helpers.h>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace bpt::refdata::adapter {

using bpt::common::util::base64_encode;
using bpt::common::util::hmac_sha256;

namespace {

std::string hmac_sha256_b64(const std::string& key, const std::string& message) {
    return base64_encode(hmac_sha256(key, message));
}

std::string iso8601_now() {
    using std::chrono::duration_cast;
    using std::chrono::milliseconds;
    using std::chrono::system_clock;
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
    return oss.str();
}

}  // namespace

http::RestClient::Headers okx_auth_headers(const std::string& api_key,
                                           const std::string& secret_key,
                                           const std::string& passphrase,
                                           const std::string& method,
                                           const std::string& target,
                                           bool simulated) {
    const std::string timestamp = iso8601_now();
    const std::string signature = hmac_sha256_b64(secret_key, timestamp + method + target);

    http::RestClient::Headers headers{
        {"OK-ACCESS-KEY", api_key},
        {"OK-ACCESS-SIGN", signature},
        {"OK-ACCESS-TIMESTAMP", timestamp},
        {"OK-ACCESS-PASSPHRASE", passphrase},
    };
    if (simulated)
        headers.emplace_back("x-simulated-trading", "1");
    return headers;
}

}  // namespace bpt::refdata::adapter
