#include "md_gateway/md/md_encoder.h"

// Use explicit SBE namespace aliases to avoid ambiguity with bpt::md_gateway::md types
// that share names (MdTrade, MdOrderBook).
namespace sbe = bpt::messages;

namespace bpt::md_gateway::md {

std::size_t MdEncoder::encode(const MdBbo& bbo, uint64_t seq_num, char* buf, std::size_t capacity) noexcept {
    if (capacity < kBboBufSize)
        return 0;

    sbe::MdMarketData msg;
    msg.wrapAndApplyHeader(buf, 0, capacity)
        .timestampNs(bbo.timestamp_ns)
        .instrumentId(bbo.instrument_id)
        .bidPrice(bbo.bid_price)
        .bidQty(bbo.bid_qty)
        .askPrice(bbo.ask_price)
        .askQty(bbo.ask_qty)
        .seqNum(seq_num);

    return kBboBufSize;
}

std::size_t MdEncoder::encode(const MdTrade& trade, uint64_t seq_num, char* buf, std::size_t capacity) noexcept {
    if (capacity < kTradeBufSize)
        return 0;

    sbe::MdTrade msg;
    msg.wrapAndApplyHeader(buf, 0, capacity)
        .timestampNs(trade.timestamp_ns)
        .instrumentId(trade.instrument_id)
        .price(trade.price)
        .qty(trade.qty)
        .side(trade.side)
        .seqNum(seq_num);

    return kTradeBufSize;
}

std::size_t MdEncoder::encode(const MdOrderBook& book, uint64_t seq_num, char* buf, std::size_t capacity) noexcept {
    auto n_bids = static_cast<uint16_t>(book.bids.size());
    auto n_asks = static_cast<uint16_t>(book.asks.size());

    if (n_bids > kMaxLevels || n_asks > kMaxLevels)
        return 0;

    std::size_t needed = sbe::MessageHeader::encodedLength() + sbe::MdOrderBook::sbeBlockLength() +
                         sbe::MdOrderBook::Bids::sbeHeaderSize() + n_bids * sbe::MdOrderBook::Bids::sbeBlockLength() +
                         sbe::MdOrderBook::Asks::sbeHeaderSize() + n_asks * sbe::MdOrderBook::Asks::sbeBlockLength();

    if (capacity < needed)
        return 0;

    sbe::MdOrderBook msg;
    msg.wrapAndApplyHeader(buf, 0, needed)
        .timestampNs(book.timestamp_ns)
        .instrumentId(book.instrument_id)
        .seqNum(seq_num);

    auto& bg = msg.bidsCount(n_bids);
    for (uint16_t i = 0; i < n_bids; ++i)
        bg.next().price(book.bids[i].first).qty(book.bids[i].second);

    auto& ag = msg.asksCount(n_asks);
    for (uint16_t i = 0; i < n_asks; ++i)
        ag.next().price(book.asks[i].first).qty(book.asks[i].second);

    return needed;
}

}  // namespace bpt::md_gateway::md
