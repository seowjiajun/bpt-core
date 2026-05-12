#include "order_gateway/adapter/binance/binance_auth.h"

#include <bpt_common/util/openssl_helpers.h>
#include <bpt_common/util/tsc_clock.h>

namespace bpt::order_gateway::adapter::binance {

using bpt::common::util::hex_encode;
using bpt::common::util::hmac_sha256;
using bpt::common::util::WallClock;

std::string hmac_sha256_hex(std::string_view key, std::string_view data) {
    return hex_encode(hmac_sha256(key, data));
}

std::string sign_query(std::string_view secret_key, const std::string& params) {
    const uint64_t ts_ms = WallClock::now_ms();
    std::string full = params + "&timestamp=" + std::to_string(ts_ms);
    std::string sig = hmac_sha256_hex(secret_key, full);
    return full + "&signature=" + sig;
}

}  // namespace bpt::order_gateway::adapter::binance
