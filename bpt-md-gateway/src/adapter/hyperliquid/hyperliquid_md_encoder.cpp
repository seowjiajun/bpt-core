#include "md_gateway/adapter/hyperliquid/hyperliquid_md_encoder.h"

#include <fmt/format.h>

namespace bpt::md_gateway::adapter::hyperliquid {

std::string build_subscribe_payload(std::string_view sub_type, const std::string& coin) {
    return fmt::format(R"({{"method":"subscribe","subscription":{{"type":"{}","coin":"{}"}}}})", sub_type, coin);
}

std::string build_ping_payload() {
    return R"({"method":"ping"})";
}

}  // namespace bpt::md_gateway::adapter::hyperliquid
