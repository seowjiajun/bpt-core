#pragma once

/// \file
/// \brief OKX MD frame decoder (JSON → SBE).

#include "md_gateway/adapter/common/json_decoder_base.h"
#include "md_gateway/adapter/common/subscription_map.h"
#include "md_gateway/md/md_types.h"
#include "md_gateway/md/sorted_ladder.h"
#include "md_gateway/messaging/publishers/funding_rate_publisher.h"
#include "md_gateway/messaging/publishers/i_instrument_stats_publisher.h"

#include <messages/ExchangeId.h>
#include <messages/TradeSide.h>

#include <bpt_common/logging.h>
#include <bpt_common/util/latency_histogram.h>
#include <bpt_common/util/parse_double.h>
#include <bpt_common/util/tsc_clock.h>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string_view>
#include <unordered_map>

namespace bpt::md_gateway::adapter {

/// \brief Decodes OKX WS frames (JSON envelope with `"arg"` + `"data"`) and publishes SBE.
///
/// Templated on Pub (concrete inner-pub type) so the publish() chain
/// inlines completely — see md_gateway/md/validating_publisher.h.
///
/// Handled channels:
///   - bbo-tbt        → handle_bbo (tick-by-tick BBO)
///   - books5 / books → handle_book (top-N ladder + derived BBO)
///   - trades         → handle_trades
///   - funding-rate   → handle_funding_rate
///
/// Channel dispatch uses a Channel enum + switch (compiler jump table)
/// instead of a string if-else chain. to_channel() maps the raw string
/// to the enum once; the main decode() path never compares strings
/// against channel names.
///
/// Uses simdjson on-demand API with pre-allocated padded buffer
/// (JsonDecoderBase) for zero-allocation hot-path parsing. Decode
/// latency (parse + field extraction + Aeron offer) is recorded into
/// decode_lat_ for Prometheus export.
template <class Pub>
class OkxMdDecoder : public JsonDecoderBase {
public:
    explicit OkxMdDecoder(const SubscriptionMap& subs) : subs_(subs) {}

    void decode(std::string_view payload,
                uint64_t recv_ns,
                Pub& pub,
                messaging::FundingRateCallback& on_funding_rate,
                messaging::InstrumentStatsCallback& /*on_instrument_stats*/) {
        const uint64_t parse_start_ns = bpt::common::util::TscClock::now_mono_ns();
        pad(payload);

        simdjson::ondemand::document doc;
        if (json_parser_.iterate(padded_buf_.data(), payload.size(), padded_buf_.size()).get(doc)) [[unlikely]]
            return;

        {
            std::string_view event_sv;
            bool has_event = !doc.find_field_unordered("event").get_string().get(event_sv);
            doc.rewind();
            if (has_event) [[unlikely]] {
                bpt::common::log::info("OKX event: {}", event_sv);
                return;
            }
        }

        std::string_view channel, inst_id;
        {
            simdjson::ondemand::object arg;
            if (doc.find_field_unordered("arg").get_object().get(arg)) [[unlikely]]
                return;
            if (arg["channel"].get_string().get(channel)) [[unlikely]]
                return;
            if (arg["instId"].get_string().get(inst_id)) [[unlikely]]
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
        if (doc.find_field_unordered("data").get_array().get(data_arr)) [[unlikely]]
            return;

        simdjson::ondemand::object entry;
        if (data_arr.at(0).get_object().get(entry)) [[unlikely]]
            return;

        // Single lock + single hash lookup for both instrument_id and depth.
        auto [instrument_id, depth] = subs_.find(inst_id);
        if (!instrument_id) [[unlikely]]
            return;

        // Classify channel with two single-char checks — no multi-char string
        // comparison on the hot path.
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

    bpt::common::util::LatencyHistogram decode_lat_;

private:
    /// \brief Channel discriminator — see decode() for the classification rules.
    enum class Channel : uint8_t { Bbo, Book, Trades, FundingRate };

    void handle_bbo(simdjson::ondemand::object& entry,
                    uint64_t instrument_id,
                    uint64_t recv_ns,
                    uint64_t parse_start_ns,
                    Pub& pub) {
        md::MdBbo bbo;
        bbo.timestamp_ns = recv_ns;
        bbo.instrument_id = instrument_id;
        {
            simdjson::ondemand::array outer, lvl;
            if (entry["bids"].get_array().get(outer)) [[unlikely]]
                return;
            if (outer.at(0).get_array().get(lvl)) [[unlikely]]
                return;
            auto it = lvl.begin();
            if (it.error()) [[unlikely]]
                return;
            (void)bpt::common::util::ff_double(*it, bbo.bid_price);
            if ((++it).error()) [[unlikely]]
                return;
            (void)bpt::common::util::ff_double(*it, bbo.bid_qty);
        }
        {
            simdjson::ondemand::array outer, lvl;
            if (entry.find_field_unordered("asks").get_array().get(outer)) [[unlikely]]
                return;
            if (outer.at(0).get_array().get(lvl)) [[unlikely]]
                return;
            auto it = lvl.begin();
            if (it.error()) [[unlikely]]
                return;
            (void)bpt::common::util::ff_double(*it, bbo.ask_price);
            if ((++it).error()) [[unlikely]]
                return;
            (void)bpt::common::util::ff_double(*it, bbo.ask_qty);
        }

        uint64_t lat_ns = bpt::common::util::TscClock::now_mono_ns() - parse_start_ns;
        decode_lat_.record(lat_ns);
        if (++tick_count_ <= 20 || tick_count_ % 500 == 0)
            bpt::common::log::info("OKX BBO decode: {}ns tick={}", lat_ns, tick_count_);
        pub.publish(bbo);
    }

    void handle_book(simdjson::ondemand::object& entry,
                     uint64_t instrument_id,
                     uint64_t recv_ns,
                     uint64_t parse_start_ns,
                     uint8_t depth,
                     bool is_snapshot,
                     Pub& pub) {
        auto& state = book_state_[instrument_id];
        if (is_snapshot) {
            state.bids.clear();
            state.asks.clear();
        }

        auto apply_side = [](simdjson::ondemand::array& outer, auto& side) {
            for (auto level_res : outer) {
                simdjson::ondemand::array lvl;
                if (level_res.get_array().get(lvl)) [[unlikely]]
                    continue;
                double px = 0, qty = 0;
                auto it = lvl.begin();
                if (it.error()) [[unlikely]]
                    continue;
                (void)bpt::common::util::ff_double(*it, px);
                if ((++it).error()) [[unlikely]]
                    continue;
                (void)bpt::common::util::ff_double(*it, qty);
                side.apply(px, qty);
            }
        };

        {
            simdjson::ondemand::array outer;
            if (entry["bids"].get_array().get(outer)) [[unlikely]]
                return;
            apply_side(outer, state.bids);
        }
        {
            simdjson::ondemand::array outer;
            if (entry.find_field_unordered("asks").get_array().get(outer)) [[unlikely]]
                return;
            apply_side(outer, state.asks);
        }

        if (state.bids.empty() || state.asks.empty()) [[unlikely]]
            return;

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

    /// \brief Per-instrument local book state for delta accumulation.
    ///
    /// Bids: descending order so begin() is the best bid.
    /// Asks: ascending order so begin() is the best ask.
    /// Each side is a contiguous SortedLadder (vector-backed, no per-level
    /// heap allocations after warmup) — see md/sorted_ladder.h.
    struct BookState {
        md::SortedLadder<std::greater<double>> bids;
        md::SortedLadder<std::less<double>> asks;
    };
    std::unordered_map<uint64_t, BookState> book_state_;

    void handle_trades(simdjson::ondemand::object& entry, uint64_t instrument_id, uint64_t recv_ns, Pub& pub) {
        md::MdTrade trade;
        trade.timestamp_ns = recv_ns;
        trade.instrument_id = instrument_id;
        if (bpt::common::util::ff_double(entry["px"], trade.price)) [[unlikely]]
            return;
        if (bpt::common::util::ff_double(entry.find_field_unordered("sz"), trade.qty)) [[unlikely]]
            return;

        std::string_view side_sv;
        if (entry.find_field_unordered("side").get_string().get(side_sv)) [[unlikely]]
            return;

        trade.side = (side_sv == "sell") ? bpt::messages::TradeSide::SELL : bpt::messages::TradeSide::BUY;
        pub.publish(trade);
    }

    void handle_funding_rate(simdjson::ondemand::object& entry,
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

    const SubscriptionMap& subs_;
    uint64_t tick_count_{0};
};

}  // namespace bpt::md_gateway::adapter
