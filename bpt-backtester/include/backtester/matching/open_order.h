#pragma once

#include <cstdint>
#include <string>

namespace bpt::backtester::matching {

enum class OrderType { MARKET, LIMIT };
enum class OrderSide { BUY, SELL };

struct OpenOrder {
    std::string order_id;
    std::string client_order_id;
    std::string exchange;
    std::string symbol;
    OrderType type{OrderType::LIMIT};
    OrderSide side{OrderSide::BUY};
    double price{0.0};  // LIMIT price; unused for MARKET
    double quantity{0.0};
    double filled_qty{0.0};
    uint64_t submitted_ts{0};
};

struct FillReport {
    std::string order_id;
    std::string client_order_id;
    std::string exchange;
    std::string symbol;
    OrderType order_type{OrderType::LIMIT};
    OrderSide side{OrderSide::BUY};
    double original_qty{0.0};
    double order_price{0.0};  // limit price of the original order
    double last_fill_qty{0.0};
    double last_fill_price{0.0};
    double cumulative_fill_qty{0.0};
    bool is_fully_filled{false};
    uint64_t simulation_ts{0};
};

}  // namespace bpt::backtester::matching
