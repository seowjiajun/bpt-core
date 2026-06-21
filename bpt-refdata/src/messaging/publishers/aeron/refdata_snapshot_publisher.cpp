#include "refdata/messaging/publishers/aeron/refdata_snapshot_publisher.h"

#include "refdata/messaging/sbe_utils.h"

#include <messages/MessageHeader.h>
#include <messages/OptionSide.h>
#include <messages/RefDataSnapshot.h>

#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>
#include <bpt_common/util/tsc_clock.h>
#include <vector>

namespace bpt::refdata::messaging::aeron {

using bpt::messages::MessageHeader;
using bpt::messages::OptionSide;
using bpt::messages::RefDataSnapshot;

namespace {

// The SBE exchange field is a fixed 8-char array. Exchange names longer than
// 8 characters get silently truncated on the wire. A filter sent by a client
// as "HYPERLIQUID" arrives here as "HYPERLIQ". This helper compares the full
// venue name (e.g. "HYPERLIQUID") against a possibly-truncated filter by
// checking that the filter is a prefix of the venue. Remove once the SBE
// schema is widened.
bool venue_matches_filter_exchange(const std::string& venue, const char* filter_exchange) {
    if (filter_exchange[0] == '\0')
        return true;
    std::size_t flen = strnlen(filter_exchange, 8);
    std::string_view fv(filter_exchange, flen);
    if (venue == fv)
        return true;
    // Prefix match — handles "HYPERLIQ" filter matching "HYPERLIQUID" venue.
    if (flen == 8 && venue.size() > 8 && venue.compare(0, 8, fv) == 0)
        return true;
    return false;
}

// Returns true if inst matches the request filter (empty filters = match all).
bool matches(const model::Instrument& inst, const RefdataRequest& req) {
    // Canonical filter takes priority when present.
    if (!req.canonical_filters.empty()) {
        for (const auto& f : req.canonical_filters) {
            bool base_ok = inst.base == std::string_view(f.base_currency, strnlen(f.base_currency, 8));
            bool quote_ok = inst.quote == std::string_view(f.quote_currency, strnlen(f.quote_currency, 8));
            bool type_ok = to_sbe_type(inst.inst_type) == f.instrument_type;
            bool ex_ok = venue_matches_filter_exchange(inst.venue, f.exchange);
            if (base_ok && quote_ok && type_ok && ex_ok)
                return true;
        }
        return false;
    }
    // Legacy venue-symbol filter.
    if (!req.instruments.empty()) {
        for (const auto& f : req.instruments) {
            bool sym_ok =
                (f.symbol[0] == '\0') || (inst.venue_symbol == std::string_view(f.symbol, strnlen(f.symbol, 24)));
            bool ex_ok = venue_matches_filter_exchange(inst.venue, f.exchange);
            if (sym_ok && ex_ok)
                return true;
        }
        return false;
    }
    return true;  // no filter — match all
}

}  // namespace

RefdataSnapshotPublisher::RefdataSnapshotPublisher(std::shared_ptr<::aeron::Aeron> aeron,
                                                   const bpt::common::config::StreamConfig& stream) {
    publication_ = bpt::common::aeron::wait_for_publication(aeron, stream.channel, stream.stream_id);
}

void RefdataSnapshotPublisher::publish(const registry::InstrumentRegistry& registry,
                                       const RefdataRequest& request,
                                       uint64_t seq_start) {
    // Pass 1: collect matching instruments (only copies matches, not the full registry).
    std::vector<model::Instrument> matched;
    registry.for_each([&](const model::Instrument& inst) {
        if (matches(inst, request))
            matched.push_back(inst);
    });

    bpt::common::log::info("Snapshot correlation_id={}: {} instrument(s)", request.correlation_id, matched.size());

    // Allocate buffer: header + fixed block + group header + N × instrument block
    std::size_t n = matched.size();
    std::size_t buf_size = MessageHeader::encodedLength() + RefDataSnapshot::sbeBlockLength() +
                           RefDataSnapshot::Instruments::sbeHeaderSize() +
                           n * RefDataSnapshot::Instruments::sbeBlockLength();

    std::vector<char> buf(buf_size, '\0');

    uint64_t now_ns = bpt::common::util::TscClock::now_epoch_ns();

    RefDataSnapshot msg;
    msg.wrapAndApplyHeader(buf.data(), 0, buf_size)
        .correlationId(request.correlation_id)
        .snapshotSeqNum(seq_start)
        .timestampNs(now_ns);

    auto& group = msg.instrumentsCount(static_cast<uint16_t>(n));
    for (const auto& inst : matched) {
        group.next().instrumentId(inst.inst_uid);

        put_str<24>(group.symbol(), inst.venue_symbol);
        put_str<8>(group.exchange(), inst.venue);
        put_str<8>(group.baseCurrency(), inst.base);
        put_str<8>(group.quoteCurrency(), inst.quote);

        group.instrumentType(to_sbe_type(inst.inst_type))
            .status(to_sbe_status(inst.status))
            .lotSize(inst.lot_size)
            .tickSize(inst.tick_size)
            .contractSize(inst.contract_multiplier)
            .expiryDate(inst.expiry_timestamp.has_value() ? ns_to_yyyymmdd(*inst.expiry_timestamp) : 0u)
            .optionSide(OptionSide::NA)
            .strikePrice(inst.strike_price.value_or(0.0));
        put_str<24>(group.underlying(), inst.base);
    }

    ::aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(buf.data()), static_cast<::aeron::util::index_t>(buf_size));
    aeron_offer(*publication_, ab, static_cast<::aeron::util::index_t>(buf_size), "snapshot");

    bpt::common::log::info("Snapshot sent correlation_id={}", request.correlation_id);
}

}  // namespace bpt::refdata::messaging::aeron
