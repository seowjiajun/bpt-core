#include "md_gateway/adapter/okx/okx_md_encoder.h"

#include <fmt/format.h>

namespace bpt::md_gateway::adapter::okx {

std::string build_subscribe_payload(const std::string& symbol, uint8_t depth) {
    const char* book_channel = (depth == 0) ? "bbo-tbt" : (depth <= 5) ? "books5" : "books";

    const bool is_swap = symbol.size() > 5 && symbol.substr(symbol.size() - 5) == "-SWAP";
    if (is_swap) {
        // For SWAP perps: also subscribe to mark-price (per-instId), index-tickers
        // (per index symbol — the underlying spot pair, e.g. BTC-USDT for
        // BTC-USDT-SWAP), and funding-rate. These feed perp PnL accounting on
        // replay; without them the backtester has no mark to value inventory at,
        // no index for funding rate calc, and no funding cashflow.
        // TODO: liquidation-orders is a global SWAP channel (subscribed by
        // instType, not instId) — handle once at connect time, not per-symbol.
        const std::string index_symbol = symbol.substr(0, symbol.size() - 5);
        return fmt::format(R"({{"op":"subscribe","args":[)"
                           R"({{"channel":"{}","instId":"{}"}},)"
                           R"({{"channel":"trades","instId":"{}"}},)"
                           R"({{"channel":"funding-rate","instId":"{}"}},)"
                           R"({{"channel":"mark-price","instId":"{}"}},)"
                           R"({{"channel":"index-tickers","instId":"{}"}}]}})",
                           book_channel,
                           symbol,
                           symbol,
                           symbol,
                           symbol,
                           index_symbol);
    }
    return fmt::format(R"({{"op":"subscribe","args":[)"
                       R"({{"channel":"{}","instId":"{}"}},)"
                       R"({{"channel":"trades","instId":"{}"}}]}})",
                       book_channel,
                       symbol,
                       symbol);
}

}  // namespace bpt::md_gateway::adapter::okx
