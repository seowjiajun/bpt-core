#include "refdata/adapter/okx/okx_refdata_auth.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <sstream>

namespace bpt::refdata::adapter {

namespace {

std::string base64_encode(const unsigned char* data, size_t len) {
    std::string out;
    out.resize(((len + 2) / 3) * 4);
    int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data, static_cast<int>(len));
    out.resize(static_cast<size_t>(written));
    return out;
}

std::string hmac_sha256_b64(const std::string& key, const std::string& message) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    HMAC(EVP_sha256(),
         key.data(),
         static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(message.data()),
         static_cast<int>(message.size()),
         digest,
         &digest_len);
    return base64_encode(digest, digest_len);
}

std::string iso8601_now() {
    using namespace std::chrono;
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
