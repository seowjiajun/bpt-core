#pragma once

/// @file
/// HarnessMdPublisher — the Pub side of the mdgw venue-decoder CRTP
/// chain, redirected at the deterministic backtest harness's
/// InProcessMdClient. Decoders fold like:
///
///   HyperliquidMdDecoder<HarnessMdPublisher> → HarnessMdPublisher
///       → InProcessMdClient::push_bbo / push_trade / push_order_book
///
/// The mdgw venue parsers stay byte-identical to live (same JSON
/// parsing path, same MdBbo/MdTrade/MdOrderBook intermediate types);
/// only the final hop changes — instead of writing the SBE encoding
/// into an Aeron log buffer, we encode into a stack buffer, wrap
/// it as a flyweight, and call the strategy's bound callback inline.
///
/// SBE encoding mirrors `bpt-md-gateway/src/messaging/md_publisher.cpp`.
/// Kept duplicated rather than abstracted because the mdgw publisher
/// is intentionally non-virtual (CRTP all the way down for inlining)
/// and the encode block is ~6 lines per type.
///
/// Header-only: every method is small enough to inline at the decoder
/// call site, and there's exactly one consumer (StrategyHarness).

#include "strategy/md/inprocess_md_client.h"

#include "md_gateway/md/md_encoder.h"
#include "md_gateway/md/md_types.h"

#include <messages/MdMarketData.h>
#include <messages/MdOrderBook.h>
#include <messages/MdTrade.h>
#include <messages/MessageHeader.h>

#include <cstddef>
#include <cstdint>

namespace bpt::backtester::harness {

class HarnessMdPublisher {
public:
    explicit HarnessMdPublisher(bpt::strategy::md::InProcessMdClient& client)
        : client_(client) {}

    void publish(const bpt::md_gateway::md::MdBbo& bbo) {
        ++seq_;
        constexpr std::size_t kBufSize =
            bpt::messages::MessageHeader::encodedLength() +
            bpt::messages::MdMarketData::sbeBlockLength();
        char buf[kBufSize]{};
        bpt::messages::MdMarketData msg;
        msg.wrapAndApplyHeader(buf, 0, kBufSize)
            .timestampNs(bbo.timestamp_ns)
            .instrumentId(bbo.instrument_id)
            .bidPrice(bbo.bid_price)
            .bidQty(bbo.bid_qty)
            .askPrice(bbo.ask_price)
            .askQty(bbo.ask_qty)
            .seqNum(seq_);
        client_.push_bbo(msg);
    }

    void publish(const bpt::md_gateway::md::MdTrade& trade) {
        ++seq_;
        constexpr std::size_t kBufSize =
            bpt::messages::MessageHeader::encodedLength() +
            bpt::messages::MdTrade::sbeBlockLength();
        char buf[kBufSize]{};
        bpt::messages::MdTrade msg;
        msg.wrapAndApplyHeader(buf, 0, kBufSize)
            .timestampNs(trade.timestamp_ns)
            .instrumentId(trade.instrument_id)
            .price(trade.price)
            .qty(trade.qty)
            .side(trade.side)
            .seqNum(seq_);
        client_.push_trade(msg);
    }

    void publish(const bpt::md_gateway::md::MdOrderBook& book) {
        ++seq_;
        // Encoder is the same one mdgw uses on the live wire, so
        // OrderBook's variable-length payload is bit-identical between
        // backtest replay and production.
        char buf[bpt::md_gateway::md::MdEncoder::kMaxOrderBookBufSize];
        const std::size_t len =
            bpt::md_gateway::md::MdEncoder::encode(book, seq_, buf, sizeof(buf));
        if (len == 0) return;
        bpt::messages::MdOrderBook msg;
        msg.wrapForDecode(buf,
                          bpt::messages::MessageHeader::encodedLength(),
                          bpt::messages::MdOrderBook::sbeBlockLength(),
                          bpt::messages::MdOrderBook::sbeSchemaVersion(),
                          len);
        client_.push_order_book(msg);
    }

    /// Drop counter — kept for API parity with mdgw's MdPublisher.
    /// The harness never drops (synchronous fan-out, no backpressure).
    [[nodiscard]] uint64_t drop_count() const { return 0; }

private:
    bpt::strategy::md::InProcessMdClient& client_;
    uint64_t seq_{0};
};

}  // namespace bpt::backtester::harness
