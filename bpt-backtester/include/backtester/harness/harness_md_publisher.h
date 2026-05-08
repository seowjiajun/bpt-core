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

#include "backtester/data/market_event.h"
#include "backtester/data/orderbook_record.h"
#include "backtester/data/trade_record.h"
#include "backtester/matching/matching_engine.h"

#include "strategy/md/inprocess_md_client.h"
#include "strategy/refdata/instrument_cache.h"

#include "md_gateway/md/md_encoder.h"
#include "md_gateway/md/md_types.h"

#include <messages/MdMarketData.h>
#include <messages/MdOrderBook.h>
#include <messages/MdTrade.h>
#include <messages/MessageHeader.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace bpt::backtester::harness {

class HarnessMdPublisher {
public:
    /// Strategy-only ctor (no matching-engine fan-out — useful for
    /// unit tests that want to assert the publisher → strategy hop).
    explicit HarnessMdPublisher(bpt::strategy::md::InProcessMdClient& client)
        : client_(client) {}

    /// Production ctor for the harness — every published OrderBook/Trade
    /// is also dispatched to the matching engine so resting LIMITs can
    /// be filled. The instrument cache resolves instrument_id back to
    /// venue (exchange, symbol) — MatchingEngine's API keys orders by
    /// strings, not by canonical id.
    HarnessMdPublisher(bpt::strategy::md::InProcessMdClient& client,
                       matching::MatchingEngine* matching,
                       const bpt::strategy::refdata::InstrumentCache* cache)
        : client_(client), matching_(matching), cache_(cache) {}

    void publish(const bpt::md_gateway::md::MdBbo& bbo) {
        ++seq_;

        // Feed matching engine. HL's `l2Book` channel produces BBO at
        // this layer (MdBbo, not MdOrderBook), so without this fan-out
        // the engine's books_ map stays empty and no resting LIMIT
        // ever fills (no crossing-LIMIT path, no queue-ahead seeding,
        // no book-cross backstop). Build a depth-1 OrderBookRecord
        // from the top-of-book — the matching engine uses bid_px[0]/
        // ask_px[0] for the cross check, which is all we have anyway.
        if (matching_ && cache_) {
            if (auto inst = cache_->get(bbo.instrument_id)) {
                bpt::backtester::data::OrderBookRecord ob;
                ob.timestamp_ns = bbo.timestamp_ns;
                ob.exchange     = inst->exchange;
                ob.symbol       = inst->symbol;
                ob.bid_px[0]    = bbo.bid_price;
                ob.bid_sz[0]    = bbo.bid_qty;
                ob.ask_px[0]    = bbo.ask_price;
                ob.ask_sz[0]    = bbo.ask_qty;
                matching_->on_market_event(
                    bpt::backtester::data::MarketEvent::from_orderbook(std::move(ob)));
            }
        }

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
        ++trade_count_;

        // Matching engine first — fills against book based on prior
        // book state aren't directly affected by trades, but the
        // existing engine has a fill_against_trade() path that drains
        // queue_ahead from resting LIMITs at the trade price. Mirrors
        // ClockMaster's ordering: market_event → matching → strategy.
        if (matching_ && cache_) {
            if (auto inst = cache_->get(trade.instrument_id)) {
                bpt::backtester::data::TradeRecord tr{
                    .timestamp_ns = trade.timestamp_ns,
                    .price        = trade.price,
                    .quantity     = trade.qty,
                    .side         = trade.side == bpt::messages::TradeSide::BUY
                                        ? bpt::backtester::data::TradeSide::BUY
                                        : bpt::backtester::data::TradeSide::SELL,
                    .exchange     = inst->exchange,
                    .symbol       = inst->symbol,
                };
                matching_->on_market_event(
                    bpt::backtester::data::MarketEvent::from_trade(std::move(tr)));
            }
        }

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
        ++orderbook_count_;

        if (matching_ && cache_) {
            if (auto inst = cache_->get(book.instrument_id)) {
                bpt::backtester::data::OrderBookRecord ob;
                ob.timestamp_ns = book.timestamp_ns;
                ob.exchange     = inst->exchange;
                ob.symbol       = inst->symbol;
                const std::size_t n_bid = std::min<std::size_t>(
                    book.bids.size(), bpt::backtester::data::kOrderBookDepth);
                for (std::size_t i = 0; i < n_bid; ++i) {
                    ob.bid_px[i] = book.bids[i].first;
                    ob.bid_sz[i] = book.bids[i].second;
                }
                const std::size_t n_ask = std::min<std::size_t>(
                    book.asks.size(), bpt::backtester::data::kOrderBookDepth);
                for (std::size_t i = 0; i < n_ask; ++i) {
                    ob.ask_px[i] = book.asks[i].first;
                    ob.ask_sz[i] = book.asks[i].second;
                }
                matching_->on_market_event(
                    bpt::backtester::data::MarketEvent::from_orderbook(std::move(ob)));
            }
        }

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

    /// Diagnostic counters — useful for confirming trades reached the
    /// matching engine when a backtest produces zero fills.
    [[nodiscard]] uint64_t trade_count() const { return trade_count_; }
    [[nodiscard]] uint64_t orderbook_count() const { return orderbook_count_; }

private:
    bpt::strategy::md::InProcessMdClient& client_;
    matching::MatchingEngine* matching_{nullptr};
    const bpt::strategy::refdata::InstrumentCache* cache_{nullptr};
    uint64_t seq_{0};
    uint64_t trade_count_{0};
    uint64_t orderbook_count_{0};
};

}  // namespace bpt::backtester::harness
