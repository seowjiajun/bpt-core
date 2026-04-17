#include "md_gateway/adapter/okx/okx_parser.h"

#include <messages/ExchangeId.h>
#include <messages/TradeSide.h>

#include <cmath>
#include <yggdrasil/logging.h>
#include <yggdrasil/util/parse_double.h>
#include <yggdrasil/util/tsc_clock.h>

namespace bpt::md_gateway::adapter {

// ── Top-level parse ───────────────────────────────────────────────────────────

void OkxParser::parse(std::string_view payload,
                      uint64_t recv_ns,
                      messaging::IMdPublisher& pub,
                      messaging::FundingRateCallback& on_funding_rate) {
    const uint64_t parse_start_ns = ygg::util::TscClock::now_mono_ns();
    pad(payload);

    simdjson::ondemand::document doc;
    if (json_parser_.iterate(padded_buf_.data(), payload.size(), padded_buf_.size()).get(doc))
        return;

    {
        std::string_view event_sv;
        bool has_event = !doc.find_field_unordered("event").get_string().get(event_sv);
        doc.rewind();
        if (has_event) {
            ygg::log::info("OKX event: {}", event_sv);
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
            handle_book(entry, instrument_id, recv_ns, parse_start_ns, depth, pub);
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
        (void)ygg::util::ff_double(*it, bbo.bid_price);
        if ((++it).error())
            return;
        (void)ygg::util::ff_double(*it, bbo.bid_qty);
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
        (void)ygg::util::ff_double(*it, bbo.ask_price);
        if ((++it).error())
            return;
        (void)ygg::util::ff_double(*it, bbo.ask_qty);
    }

    uint64_t lat_ns = ygg::util::TscClock::now_mono_ns() - parse_start_ns;
    decode_lat_.record(lat_ns);
    if (++tick_count_ <= 20 || tick_count_ % 500 == 0)
        ygg::log::info("OKX BBO decode: {}ns tick={}", lat_ns, tick_count_);
    pub.publish(bbo);
}

void OkxParser::handle_book(simdjson::ondemand::object& entry,
                            uint64_t instrument_id,
                            uint64_t recv_ns,
                            uint64_t parse_start_ns,
                            uint8_t depth,
                            messaging::IMdPublisher& pub) {
    // books5 / books: top-N levels.  depth comes from the subscription map,
    // already resolved by the caller — no second lookup needed.
    md::MdOrderBook book;
    book.timestamp_ns = recv_ns;
    book.instrument_id = instrument_id;
    book.bids.reserve(depth);
    book.asks.reserve(depth);

    {
        simdjson::ondemand::array outer;
        if (entry["bids"].get_array().get(outer))
            return;
        for (auto level_res : outer) {
            if (book.bids.size() >= depth)
                break;
            simdjson::ondemand::array lvl;
            if (level_res.get_array().get(lvl))
                continue;
            double px = 0, qty = 0;
            auto it = lvl.begin();
            if (it.error())
                continue;
            (void)ygg::util::ff_double(*it, px);
            if ((++it).error())
                continue;
            (void)ygg::util::ff_double(*it, qty);
            book.bids.emplace_back(px, qty);
        }
    }
    {
        simdjson::ondemand::array outer;
        if (entry.find_field_unordered("asks").get_array().get(outer))
            return;
        for (auto level_res : outer) {
            if (book.asks.size() >= depth)
                break;
            simdjson::ondemand::array lvl;
            if (level_res.get_array().get(lvl))
                continue;
            double px = 0, qty = 0;
            auto it = lvl.begin();
            if (it.error())
                continue;
            (void)ygg::util::ff_double(*it, px);
            if ((++it).error())
                continue;
            (void)ygg::util::ff_double(*it, qty);
            book.asks.emplace_back(px, qty);
        }
    }

    if (book.bids.empty() || book.asks.empty())
        return;

    uint64_t lat_ns = ygg::util::TscClock::now_mono_ns() - parse_start_ns;
    decode_lat_.record(lat_ns);
    if (++tick_count_ <= 20 || tick_count_ % 500 == 0)
        ygg::log::info("OKX book decode: {}ns tick={}", lat_ns, tick_count_);

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
    if (ygg::util::ff_double(entry["px"], trade.price))
        return;
    if (ygg::util::ff_double(entry.find_field_unordered("sz"), trade.qty))
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
    (void)ygg::util::ff_double(entry["fundingRate"], rate);

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
