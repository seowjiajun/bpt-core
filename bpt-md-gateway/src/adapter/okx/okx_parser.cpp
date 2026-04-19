#include "md_gateway/adapter/okx/okx_parser.h"

#include <messages/ExchangeId.h>
#include <messages/TradeSide.h>

#include <cmath>
#include <bpt_common/logging.h>
#include <bpt_common/util/parse_double.h>
#include <bpt_common/util/tsc_clock.h>

namespace bpt::md_gateway::adapter {

// ── Top-level parse ───────────────────────────────────────────────────────────

void OkxParser::parse(std::string_view payload,
                      uint64_t recv_ns,
                      messaging::IMdPublisher& pub,
                      messaging::FundingRateCallback& on_funding_rate) {
    const uint64_t parse_start_ns = bpt::common::util::TscClock::now_mono_ns();
    pad(payload);

    simdjson::ondemand::document doc;
    if (json_parser_.iterate(padded_buf_.data(), payload.size(), padded_buf_.size()).get(doc))
        return;

    {
        std::string_view event_sv;
        bool has_event = !doc.find_field_unordered("event").get_string().get(event_sv);
        doc.rewind();
        if (has_event) {
            bpt::common::log::info("OKX event: {}", event_sv);
            return;
        }
    }

    std::string_view channel, inst_id;
    {
        simdjson::ondemand::object arg;
        if (doc.find_field_unordered("arg").get_object().get(arg))
            return;
        if (arg["channel"].get_string().get(channel))
            return;
        if (arg["instId"].get_string().get(inst_id))
            return;
    }

    // `action` is present on the `books` channel (snapshot | update) and
    // on `books5` (always snapshot). Absent on bbo-tbt / trades /
    // funding-rate. Treat missing as snapshot — safe default since
    // those channels don't carry deltas.
    bool is_snapshot = true;
    {
        std::string_view action_sv;
        if (!doc.find_field_unordered("action").get_string().get(action_sv))
            is_snapshot = (action_sv == "snapshot");
    }

    simdjson::ondemand::array data_arr;
    if (doc.find_field_unordered("data").get_array().get(data_arr))
        return;

    simdjson::ondemand::object entry;
    if (data_arr.at(0).get_object().get(entry))
        return;

    // Single lock + single hash lookup for both instrument_id and depth.
    auto [instrument_id, depth] = subs_.find(inst_id);
    if (!instrument_id)
        return;

    // Classify channel with two single-char checks — no multi-char string
    // comparison on the hot path.  The book channel type (Bbo vs Book) is
    // determined from depth, which the adapter stored at subscribe time.
    //   channel[0] == 't' → "trades"
    //   channel[0] == 'f' → "funding-rate"
    //   else + depth == 0 → "bbo-tbt"
    //   else + depth >  0 → "books5" / "books"
    Channel ch;
    if (channel[0] == 't')
        ch = Channel::Trades;
    else if (channel[0] == 'f')
        ch = Channel::FundingRate;
    else
        ch = (depth == 0) ? Channel::Bbo : Channel::Book;

    switch (ch) {
        case Channel::Bbo:
            handle_bbo(entry, instrument_id, recv_ns, parse_start_ns, pub);
            break;
        case Channel::Book:
            handle_book(entry, instrument_id, recv_ns, parse_start_ns, depth, is_snapshot, pub);
            break;
        case Channel::Trades:
            handle_trades(entry, instrument_id, recv_ns, pub);
            break;
        case Channel::FundingRate:
            handle_funding_rate(entry, instrument_id, recv_ns, on_funding_rate);
            break;
    }
}

// ── Channel handlers ──────────────────────────────────────────────────────────

void OkxParser::handle_bbo(simdjson::ondemand::object& entry,
                           uint64_t instrument_id,
                           uint64_t recv_ns,
                           uint64_t parse_start_ns,
                           messaging::IMdPublisher& pub) {
    // bbo-tbt: one bid and one ask level — [price_str, qty_str, ...]
    md::MdBbo bbo;
    bbo.timestamp_ns = recv_ns;
    bbo.instrument_id = instrument_id;
    {
        simdjson::ondemand::array outer, lvl;
        if (entry["bids"].get_array().get(outer))
            return;
        if (outer.at(0).get_array().get(lvl))
            return;
        auto it = lvl.begin();
        if (it.error())
            return;
        (void)bpt::common::util::ff_double(*it, bbo.bid_price);
        if ((++it).error())
            return;
        (void)bpt::common::util::ff_double(*it, bbo.bid_qty);
    }
    {
        simdjson::ondemand::array outer, lvl;
        if (entry.find_field_unordered("asks").get_array().get(outer))
            return;
        if (outer.at(0).get_array().get(lvl))
            return;
        auto it = lvl.begin();
        if (it.error())
            return;
        (void)bpt::common::util::ff_double(*it, bbo.ask_price);
        if ((++it).error())
            return;
        (void)bpt::common::util::ff_double(*it, bbo.ask_qty);
    }

    uint64_t lat_ns = bpt::common::util::TscClock::now_mono_ns() - parse_start_ns;
    decode_lat_.record(lat_ns);
    if (++tick_count_ <= 20 || tick_count_ % 500 == 0)
        bpt::common::log::info("OKX BBO decode: {}ns tick={}", lat_ns, tick_count_);
    pub.publish(bbo);
}

void OkxParser::handle_book(simdjson::ondemand::object& entry,
                            uint64_t instrument_id,
                            uint64_t recv_ns,
                            uint64_t parse_start_ns,
                            uint8_t depth,
                            bool is_snapshot,
                            messaging::IMdPublisher& pub) {
    // Apply snapshot-or-update against the per-instrument local state,
    // then publish the top `depth` levels + derived BBO from that
    // maintained book. For `books5` every message is a snapshot (action
    // is always "snapshot"), so the state is effectively replaced each
    // tick — no delta accumulation. For `books` the first message is a
    // snapshot and subsequent messages carry updates; qty==0 at a level
    // means "remove that level".
    auto& state = book_state_[instrument_id];
    if (is_snapshot) {
        state.bids.clear();
        state.asks.clear();
    }

    auto apply_side = [](simdjson::ondemand::array& outer, auto& side) {
        for (auto level_res : outer) {
            simdjson::ondemand::array lvl;
            if (level_res.get_array().get(lvl))
                continue;
            double px = 0, qty = 0;
            auto it = lvl.begin();
            if (it.error())
                continue;
            (void)bpt::common::util::ff_double(*it, px);
            if ((++it).error())
                continue;
            (void)bpt::common::util::ff_double(*it, qty);
            if (qty == 0.0)
                side.erase(px);
            else
                side[px] = qty;
        }
    };

    {
        simdjson::ondemand::array outer;
        if (entry["bids"].get_array().get(outer))
            return;
        apply_side(outer, state.bids);
    }
    {
        simdjson::ondemand::array outer;
        if (entry.find_field_unordered("asks").get_array().get(outer))
            return;
        apply_side(outer, state.asks);
    }

    if (state.bids.empty() || state.asks.empty())
        return;

    // Flatten top-`depth` of the maintained state into a fresh MdOrderBook
    // for downstream. std::map iteration is in sort order (bids already
    // descending via std::greater, asks ascending), so begin() is the top.
    md::MdOrderBook book;
    book.timestamp_ns = recv_ns;
    book.instrument_id = instrument_id;
    book.bids.reserve(depth);
    book.asks.reserve(depth);
    for (auto it = state.bids.begin(); it != state.bids.end() && book.bids.size() < depth; ++it)
        book.bids.emplace_back(it->first, it->second);
    for (auto it = state.asks.begin(); it != state.asks.end() && book.asks.size() < depth; ++it)
        book.asks.emplace_back(it->first, it->second);

    uint64_t lat_ns = bpt::common::util::TscClock::now_mono_ns() - parse_start_ns;
    decode_lat_.record(lat_ns);
    if (++tick_count_ <= 20 || tick_count_ % 500 == 0)
        bpt::common::log::info("OKX book decode: {}ns tick={}", lat_ns, tick_count_);

    md::MdBbo bbo{recv_ns,
                  instrument_id,
                  book.bids[0].first,
                  book.bids[0].second,
                  book.asks[0].first,
                  book.asks[0].second};
    pub.publish(book);
    pub.publish(bbo);
}

void OkxParser::handle_trades(simdjson::ondemand::object& entry,
                              uint64_t instrument_id,
                              uint64_t recv_ns,
                              messaging::IMdPublisher& pub) {
    md::MdTrade trade;
    trade.timestamp_ns = recv_ns;
    trade.instrument_id = instrument_id;
    if (bpt::common::util::ff_double(entry["px"], trade.price))
        return;
    if (bpt::common::util::ff_double(entry.find_field_unordered("sz"), trade.qty))
        return;

    std::string_view side_sv;
    if (entry.find_field_unordered("side").get_string().get(side_sv))
        return;

    trade.side = (side_sv == "sell") ? bpt::messages::TradeSide::SELL : bpt::messages::TradeSide::BUY;
    pub.publish(trade);
}

void OkxParser::handle_funding_rate(simdjson::ondemand::object& entry,
                                    uint64_t instrument_id,
                                    uint64_t recv_ns,
                                    messaging::FundingRateCallback& on_funding_rate) {
    if (!on_funding_rate)
        return;

    double rate = 0.0;
    (void)bpt::common::util::ff_double(entry["fundingRate"], rate);

    uint64_t next_ms = 0;
    (void)entry.find_field_unordered("nextFundingTime").get_uint64_in_string().get(next_ms);

    messaging::FundingRateUpdate fr;
    fr.instrument_id = instrument_id;
    fr.exchange_id = bpt::messages::ExchangeId::OKX;
    fr.rate_bps = static_cast<int32_t>(std::round(rate * 1'000'000.0));
    fr.next_funding_ts_ns = next_ms * 1'000'000ULL;
    fr.collected_ts_ns = recv_ns;
    on_funding_rate(fr);
}

}  // namespace bpt::md_gateway::adapter
