#pragma once

#include "md_gateway/md/md_types.h"
#include "md_gateway/messaging/i_md_publisher.h"

#include <optional>

namespace bpt::md_gateway::test {

class FakeMdPublisher : public messaging::IMdPublisher {
public:
    void publish(const md::MdBbo& bbo) override {
        last_bbo = bbo;
        ++bbo_count;
    }

    void publish(const md::MdTrade& trade) override {
        last_trade = trade;
        ++trade_count;
    }

    void publish(const md::MdOrderBook& book) override {
        last_order_book = book;
        ++order_book_count;
    }

    void reset() {
        last_bbo.reset();
        last_trade.reset();
        last_order_book.reset();
        bbo_count = 0;
        trade_count = 0;
        order_book_count = 0;
    }

    std::optional<md::MdBbo> last_bbo;
    std::optional<md::MdTrade> last_trade;
    std::optional<md::MdOrderBook> last_order_book;
    int bbo_count{0};
    int trade_count{0};
    int order_book_count{0};
};

}  // namespace bpt::md_gateway::test
