#pragma once

#include "backtester/data/orderbook_record.h"
#include "backtester/data/trade_record.h"

#include <cstdint>
#include <variant>

namespace bpt::backtester::data {

struct MarketEvent {
    enum class Type { TRADE, ORDER_BOOK };

    Type type;
    uint64_t timestamp_ns;  // hoisted for cheap heap comparison

    std::variant<TradeRecord, OrderBookRecord> payload;

    // Min-heap ordering by timestamp.
    bool operator>(const MarketEvent& other) const { return timestamp_ns > other.timestamp_ns; }

    static MarketEvent from_trade(TradeRecord t) {
        uint64_t ts = t.timestamp_ns;
        return MarketEvent{Type::TRADE, ts, std::move(t)};
    }

    static MarketEvent from_orderbook(OrderBookRecord ob) {
        uint64_t ts = ob.timestamp_ns;
        return MarketEvent{Type::ORDER_BOOK, ts, std::move(ob)};
    }
};

}  // namespace bpt::backtester::data
