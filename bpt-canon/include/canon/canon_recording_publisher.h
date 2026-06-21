#pragma once

/// \file
/// \brief CanonRecordingPublisher — MdSink that writes events to canon.
///
/// Same shape as the live `MdPublisher` and the harness's
/// `HarnessMdPublisher`: implements the `publish(MdBbo)` / `publish(MdTrade)`
/// / `publish(MdOrderBook)` overloads required by `md::MdSink`. Each
/// publish SBE-encodes the event and forwards the bytes to a
/// `CanonWriter` with the right `EventType` tag.
///
/// Header-only — small enough that templated venue decoders inline the
/// dispatch site, same as the production publisher chain.

#include "canon/canon_format.h"
#include "canon/canon_sbe.h"
#include "canon/canon_writer.h"
#include "md_gateway/md/md_publisher_concept.h"
#include "md_gateway/md/md_types.h"
#include "md_gateway/messaging/publishers/api/funding_rate_publisher.h"

#include <cstdint>

namespace bpt::canon {

class CanonRecordingPublisher {
public:
    /// \param writer non-owning. Caller is responsible for `open()`-ing
    ///        it before the first publish and for outliving the
    ///        publisher.
    explicit CanonRecordingPublisher(CanonWriter& writer) noexcept : writer_(writer) {}

    void publish(const bpt::md_gateway::md::MdBbo& bbo) {
        char buf[CanonScratch::kBboSize];
        const std::size_t n = encode_bbo(bbo, next_seq(), buf, sizeof(buf));
        if (n == 0)
            return;
        writer_.write_event(bbo.timestamp_ns, EventType::BBO, std::string_view{buf, n});
        ++bbo_count_;
    }

    void publish(const bpt::md_gateway::md::MdTrade& trade) {
        char buf[CanonScratch::kTradeSize];
        const std::size_t n = encode_trade(trade, next_seq(), buf, sizeof(buf));
        if (n == 0)
            return;
        writer_.write_event(trade.timestamp_ns, EventType::TRADE, std::string_view{buf, n});
        ++trade_count_;
    }

    void publish(const bpt::md_gateway::md::MdOrderBook& book) {
        char buf[CanonScratch::kBookSize];
        const std::size_t n = encode_book(book, next_seq(), buf, sizeof(buf));
        if (n == 0)
            return;
        writer_.write_event(book.timestamp_ns, EventType::BOOK, std::string_view{buf, n});
        ++book_count_;
    }

    /// Outside the MdSink concept — used by the funding-rate callback path
    /// the venue decoder takes (separate from BBO/Trade/Book).
    void publish(const bpt::md_gateway::messaging::FundingRateUpdate& fr) {
        char buf[CanonScratch::kFundingSize];
        const std::size_t n = encode_funding(fr, buf, sizeof(buf));
        if (n == 0)
            return;
        writer_.write_event(fr.collected_ts_ns, EventType::FUNDING, std::string_view{buf, n});
        ++funding_count_;
    }

    /// Required by md::MdPublisher concept — replay never drops, always 0.
    [[nodiscard]] uint64_t drop_count() const noexcept { return 0; }

    [[nodiscard]] uint64_t bbo_count() const noexcept { return bbo_count_; }
    [[nodiscard]] uint64_t trade_count() const noexcept { return trade_count_; }
    [[nodiscard]] uint64_t book_count() const noexcept { return book_count_; }
    [[nodiscard]] uint64_t funding_count() const noexcept { return funding_count_; }

private:
    uint64_t next_seq() noexcept { return ++seq_; }

    CanonWriter& writer_;
    uint64_t seq_{0};
    uint64_t bbo_count_{0};
    uint64_t trade_count_{0};
    uint64_t book_count_{0};
    uint64_t funding_count_{0};
};

// Self-verify against the production concept so a method-signature drift
// in either MdPublisher or this class surfaces at compile time.
static_assert(bpt::md_gateway::md::MdPublisher<CanonRecordingPublisher>);

}  // namespace bpt::canon
