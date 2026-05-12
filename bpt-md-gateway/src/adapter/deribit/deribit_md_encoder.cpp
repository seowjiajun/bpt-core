#include "md_gateway/adapter/deribit/deribit_md_encoder.h"

#include <fmt/format.h>

namespace bpt::md_gateway::adapter::deribit {

std::string build_subscribe_rpc(uint64_t rpc_id, const std::string& symbol, uint8_t depth) {
    const std::string book_channel =
        (depth == 0) ? fmt::format("quote.{}", symbol) : fmt::format("book.{}.100ms", symbol);

    // ticker.{instrument}.100ms carries mark_price, index_price, current_funding
    // (perps) / funding_8h, open_interest, and last_price all in one channel —
    // everything the backtester needs for perp PnL marking + funding cashflow.
    // For options, ticker also carries Greeks / IV; for futures, basis info.
    // TODO: for full options-surface capture, switch to markprice.options.{currency}
    // (currency-multiplexed) once we run an options strategy that needs it.
    return fmt::format(R"({{"jsonrpc":"2.0","id":{},"method":"public/subscribe","params":{{"channels":[)"
                       R"("trades.{}.100ms",)"
                       R"("ticker.{}.100ms",)"
                       R"("{}"]}}}})",
                       rpc_id,
                       symbol,
                       symbol,
                       book_channel);
}

std::string build_set_heartbeat_rpc(uint64_t rpc_id, int interval_s) {
    return fmt::format(R"({{"jsonrpc":"2.0","id":{},"method":"public/set_heartbeat","params":{{"interval":{}}}}})",
                       rpc_id,
                       interval_s);
}

std::string build_test_response_rpc(uint64_t rpc_id) {
    return fmt::format(R"({{"jsonrpc":"2.0","id":{},"method":"public/test","params":{{}}}})", rpc_id);
}

}  // namespace bpt::md_gateway::adapter::deribit
