#include "canon/canon_sbe.h"

#include "md_gateway/md/md_encoder.h"

#include <messages/FundingRate.h>
#include <messages/MdMarketData.h>
#include <messages/MdTrade.h>
#include <messages/MessageHeader.h>

#include <cstring>

namespace bpt::canon {

namespace sbe = bpt::messages;

std::size_t encode_bbo(const bpt::md_gateway::md::MdBbo& bbo,
                       uint64_t seq_num,
                       char* buf,
                       std::size_t capacity) noexcept {
    const std::size_t needed = sbe::MdMarketData::sbeBlockAndHeaderLength();
    if (capacity < needed)
        return 0;
    std::memset(buf, 0, needed);

    sbe::MdMarketData msg;
    msg.wrapAndApplyHeader(buf, 0, needed)
        .timestampNs(bbo.timestamp_ns)
        .instrumentId(bbo.instrument_id)
        .bidPrice(bbo.bid_price)
        .bidQty(bbo.bid_qty)
        .askPrice(bbo.ask_price)
        .askQty(bbo.ask_qty)
        .seqNum(seq_num);
    return needed;
}

std::size_t encode_trade(const bpt::md_gateway::md::MdTrade& trade,
                         uint64_t seq_num,
                         char* buf,
                         std::size_t capacity) noexcept {
    const std::size_t needed = sbe::MdTrade::sbeBlockAndHeaderLength();
    if (capacity < needed)
        return 0;
    std::memset(buf, 0, needed);

    sbe::MdTrade msg;
    msg.wrapAndApplyHeader(buf, 0, needed)
        .timestampNs(trade.timestamp_ns)
        .instrumentId(trade.instrument_id)
        .price(trade.price)
        .qty(trade.qty)
        .side(trade.side)
        .seqNum(seq_num);
    return needed;
}

std::size_t encode_book(const bpt::md_gateway::md::MdOrderBook& book,
                        uint64_t seq_num,
                        char* buf,
                        std::size_t capacity) noexcept {
    return bpt::md_gateway::md::MdEncoder::encode(book, seq_num, buf, capacity);
}

std::size_t encode_funding(const bpt::md_gateway::messaging::FundingRateUpdate& fr,
                           char* buf,
                           std::size_t capacity) noexcept {
    const std::size_t needed = sbe::MessageHeader::encodedLength() + sbe::FundingRate::sbeBlockLength();
    if (capacity < needed)
        return 0;
    std::memset(buf, 0, needed);

    sbe::FundingRate msg;
    msg.wrapAndApplyHeader(buf, 0, needed)
        .exchangeId(fr.exchange_id)
        .instrumentId(fr.instrument_id)
        .rateBps(fr.rate_bps)
        .nextFundingTs(fr.next_funding_ts_ns)
        .collectedTs(fr.collected_ts_ns);
    return needed;
}

namespace {

/// Verify the SBE message header, then return true if the template id
/// matches `expected` and the buffer is long enough to wrap the block.
template <typename MsgT>
bool wrap_for_decode_header(const char* buf,
                            std::size_t len,
                            sbe::MessageHeader& hdr,
                            std::uint16_t expected) noexcept {
    if (len < sbe::MessageHeader::encodedLength())
        return false;
    hdr.wrap(const_cast<char*>(buf), 0, MsgT::sbeSchemaVersion(), len);
    if (hdr.templateId() != expected)
        return false;
    if (len < sbe::MessageHeader::encodedLength() + hdr.blockLength())
        return false;
    return true;
}

}  // namespace

bool decode_bbo(const char* buf, std::size_t len, bpt::md_gateway::md::MdBbo& out) noexcept {
    sbe::MessageHeader hdr;
    if (!wrap_for_decode_header<sbe::MdMarketData>(buf, len, hdr, sbe::MdMarketData::sbeTemplateId()))
        return false;
    sbe::MdMarketData msg;
    msg.wrapForDecode(const_cast<char*>(buf),
                      sbe::MessageHeader::encodedLength(),
                      hdr.blockLength(),
                      hdr.version(),
                      len);
    out.timestamp_ns = msg.timestampNs();
    out.instrument_id = msg.instrumentId();
    out.bid_price = msg.bidPrice();
    out.bid_qty = msg.bidQty();
    out.ask_price = msg.askPrice();
    out.ask_qty = msg.askQty();
    return true;
}

bool decode_trade(const char* buf, std::size_t len, bpt::md_gateway::md::MdTrade& out) noexcept {
    sbe::MessageHeader hdr;
    if (!wrap_for_decode_header<sbe::MdTrade>(buf, len, hdr, sbe::MdTrade::sbeTemplateId()))
        return false;
    sbe::MdTrade msg;
    msg.wrapForDecode(const_cast<char*>(buf),
                      sbe::MessageHeader::encodedLength(),
                      hdr.blockLength(),
                      hdr.version(),
                      len);
    out.timestamp_ns = msg.timestampNs();
    out.instrument_id = msg.instrumentId();
    out.price = msg.price();
    out.qty = msg.qty();
    out.side = msg.side();
    return true;
}

bool decode_book(const char* buf, std::size_t len, bpt::md_gateway::md::MdOrderBook& out) noexcept {
    sbe::MessageHeader hdr;
    if (!wrap_for_decode_header<sbe::MdOrderBook>(buf, len, hdr, sbe::MdOrderBook::sbeTemplateId()))
        return false;
    sbe::MdOrderBook msg;
    msg.wrapForDecode(const_cast<char*>(buf),
                      sbe::MessageHeader::encodedLength(),
                      hdr.blockLength(),
                      hdr.version(),
                      len);
    out.timestamp_ns = msg.timestampNs();
    out.instrument_id = msg.instrumentId();
    out.bids.clear();
    out.asks.clear();

    auto& bids = msg.bids();
    while (bids.hasNext()) {
        bids.next();
        if (out.bids.size() >= bpt::md_gateway::md::kMaxBookLevels)
            break;  // schema mismatch — encoder enforces this cap too
        out.bids.emplace_back(bids.price(), bids.qty());
    }
    auto& asks = msg.asks();
    while (asks.hasNext()) {
        asks.next();
        if (out.asks.size() >= bpt::md_gateway::md::kMaxBookLevels)
            break;
        out.asks.emplace_back(asks.price(), asks.qty());
    }
    return true;
}

bool decode_funding(const char* buf, std::size_t len, bpt::md_gateway::messaging::FundingRateUpdate& out) noexcept {
    sbe::MessageHeader hdr;
    if (!wrap_for_decode_header<sbe::FundingRate>(buf, len, hdr, sbe::FundingRate::sbeTemplateId()))
        return false;
    sbe::FundingRate msg;
    msg.wrapForDecode(const_cast<char*>(buf),
                      sbe::MessageHeader::encodedLength(),
                      hdr.blockLength(),
                      hdr.version(),
                      len);
    out.exchange_id = msg.exchangeId();
    out.instrument_id = msg.instrumentId();
    out.rate_bps = msg.rateBps();
    out.next_funding_ts_ns = msg.nextFundingTs();
    out.collected_ts_ns = msg.collectedTs();
    return true;
}

}  // namespace bpt::canon
