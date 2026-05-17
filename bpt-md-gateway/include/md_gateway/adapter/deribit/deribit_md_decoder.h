#pragma once

/// \file
/// \brief Deribit MD frame decoder (JSON-RPC → SBE).

#include "md_gateway/adapter/common/json_decoder_base.h"
#include "md_gateway/adapter/common/subscription_map.h"
#include "md_gateway/md/md_publisher_concept.h"
#include "md_gateway/md/md_types.h"
#include "md_gateway/messaging/publishers/api/funding_rate_publisher.h"
#include "md_gateway/messaging/publishers/api/instrument_stats_publisher.h"

#include <messages/TradeSide.h>

#include <atomic>
#include <bpt_common/logging.h>
#include <bpt_common/util/latency_histogram.h>
#include <bpt_common/util/tsc_clock.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace bpt::md_gateway::adapter {

/// \brief Decodes Deribit JSON-RPC 2.0 WS frames and publishes SBE.
///
/// Templated on Pub for vtable-free publish().
///
/// Handled methods:
///   - `subscription` → dispatches on channel prefix (quote / trades / book)
///   - `heartbeat`    → sets test_request_pending flag for the adapter read loop
///
/// Per-session state (reset on each reconnect via reset()):
///   - last_change_id_ — tracks book change IDs for gap detection.
///   - On a gap, the affected instrument is re-queued via subs_.requeue().
///
/// test_request_pending is set from decode() and consumed by the adapter
/// read loop via take_test_request(). Both are called sequentially on the
/// IO thread so no locking is needed.
template <md::MdSink Pub>
class DeribitMdDecoder : public JsonDecoderBase {
public:
    explicit DeribitMdDecoder(SubscriptionMap& subs) : subs_(subs) {}

    void decode(std::string_view payload,
                uint64_t recv_ns,
                Pub& pub,
                messaging::FundingRateCallback& on_funding_rate,
                messaging::InstrumentStatsCallback& on_instrument_stats) {
        const uint64_t parse_start_ns = bpt::common::util::TscClock::now_mono_ns();
        pad(payload);

        simdjson::ondemand::document doc;
        if (json_parser_.iterate(padded_buf_.data(), payload.size(), padded_buf_.size()).get(doc)) [[unlikely]]
            return;

        std::string_view method;
        if (doc["method"].get_string().get(method)) [[unlikely]]
            return;

        // --- Heartbeat test_request ---
        if (method == "heartbeat") [[unlikely]] {
            simdjson::ondemand::object params;
            if (doc.find_field_unordered("params").get_object().get(params))
                return;
            std::string_view type;
            if (!params["type"].get_string().get(type) && type == "test_request")
                test_request_pending_ = true;
            return;
        }

        if (method != "subscription") [[unlikely]]
            return;

        simdjson::ondemand::object params;
        if (doc.find_field_unordered("params").get_object().get(params)) [[unlikely]]
            return;

        std::string_view channel;
        if (params["channel"].get_string().get(channel)) [[unlikely]]
            return;

        simdjson::ondemand::value data_val;
        if (params.find_field_unordered("data").get(data_val)) [[unlikely]]
            return;

        // Extract instrument symbol from channel name.
        std::string_view symbol;
        if (channel.starts_with("quote.")) {
            symbol = channel.substr(6);
        } else if (channel.starts_with("trades.")) {
            auto last_dot = channel.rfind('.');
            symbol = (last_dot > 7) ? channel.substr(7, last_dot - 7) : channel.substr(7);
        } else if (channel.starts_with("book.")) {
            auto last_dot = channel.rfind('.');
            symbol = (last_dot > 5) ? channel.substr(5, last_dot - 5) : channel.substr(5);
        } else if (channel.starts_with("ticker.")) {
            // ticker.{symbol}.100ms — strip the prefix and the trailing ".100ms" cadence suffix
            auto last_dot = channel.rfind('.');
            symbol = (last_dot > 7) ? channel.substr(7, last_dot - 7) : channel.substr(7);
        } else {
            return;
        }

        uint64_t instrument_id = subs_.find_id(symbol);
        if (!instrument_id) [[unlikely]]
            return;

        uint8_t depth = subs_.find_depth(instrument_id);

        // --- Quote (BBO) ---
        if (channel.starts_with("quote.")) {
            simdjson::ondemand::object data;
            if (data_val.get_object().get(data)) [[unlikely]]
                return;

            md::MdBbo bbo;
            bbo.timestamp_ns = recv_ns;
            bbo.instrument_id = instrument_id;
            if (data["best_bid_price"].get_double().get(bbo.bid_price)) [[unlikely]]
                return;
            if (data.find_field_unordered("best_bid_amount").get_double().get(bbo.bid_qty)) [[unlikely]]
                return;
            if (data.find_field_unordered("best_ask_price").get_double().get(bbo.ask_price)) [[unlikely]]
                return;
            if (data.find_field_unordered("best_ask_amount").get_double().get(bbo.ask_qty)) [[unlikely]]
                return;

            uint64_t lat_ns = bpt::common::util::TscClock::now_mono_ns() - parse_start_ns;
            decode_lat_.record(lat_ns);
            if (++tick_count_ <= 20 || tick_count_ % 500 == 0)
                bpt::common::log::info("Deribit BBO decode: {}ns tick={}", lat_ns, tick_count_);
            pub.publish(bbo);
            return;
        }

        // --- Trades ---
        if (channel.starts_with("trades.")) {
            simdjson::ondemand::array trades;
            if (data_val.get_array().get(trades))
                return;
            for (auto trade_res : trades) {
                simdjson::ondemand::object obj;
                if (trade_res.get_object().get(obj))
                    continue;

                md::MdTrade trade;
                trade.timestamp_ns = recv_ns;
                trade.instrument_id = instrument_id;
                (void)obj["price"].get_double().get(trade.price);
                (void)obj.find_field_unordered("amount").get_double().get(trade.qty);

                std::string_view direction;
                (void)obj.find_field_unordered("direction").get_string().get(direction);
                trade.side = (direction == "sell") ? bpt::messages::TradeSide::SELL : bpt::messages::TradeSide::BUY;
                pub.publish(trade);
            }
            return;
        }

        // --- Order Book ---
        if (channel.starts_with("book.")) {
            simdjson::ondemand::object data;
            if (data_val.get_object().get(data))
                return;

            std::string_view book_type;
            bool is_snapshot = false;
            if (!data["type"].get_string().get(book_type))
                is_snapshot = (book_type == "snapshot");

            // Gap detection.
            if (!is_snapshot) {
                uint64_t prev_change_id = 0;
                if (!data.find_field_unordered("prev_change_id").get_uint64().get(prev_change_id)) {
                    std::string sym_key(symbol);
                    auto lcid_it = last_change_id_.find(sym_key);
                    if (lcid_it != last_change_id_.end() && lcid_it->second != prev_change_id) {
                        bpt::common::log::warn("DeribitMdDecoder: book gap for {} (expected={} got={}), resubscribing",
                                               sym_key,
                                               lcid_it->second,
                                               prev_change_id);
                        last_change_id_.erase(lcid_it);
                        subs_.requeue(sym_key);
                        return;
                    }
                }
            }

            {
                uint64_t change_id = 0;
                std::string sym_key(symbol);
                if (!data.find_field_unordered("change_id").get_uint64().get(change_id))
                    last_change_id_[sym_key] = change_id;
            }

            uint16_t max_levels = (depth > 0) ? depth : 1;

            md::MdOrderBook book;
            book.timestamp_ns = recv_ns;
            book.instrument_id = instrument_id;
            book.bids.reserve(max_levels);
            book.asks.reserve(max_levels);

            auto parse_levels = [&](simdjson::ondemand::array src, md::MdOrderBook::Levels& dst) {
                for (auto lvl_res : src) {
                    if (dst.size() >= max_levels)
                        break;
                    simdjson::ondemand::array lvl;
                    if (lvl_res.get_array().get(lvl))
                        continue;

                    auto it = lvl.begin();
                    if (it.error())
                        continue;

                    if (is_snapshot) {
                        double px = 0, qty = 0;
                        (void)(*it).get_double().get(px);
                        if ((++it).error())
                            continue;
                        (void)(*it).get_double().get(qty);
                        dst.emplace_back(px, qty);
                    } else {
                        std::string_view action;
                        if ((*it).get_string().get(action))
                            continue;
                        if (action == "delete")
                            continue;
                        if ((++it).error())
                            continue;
                        double px = 0, qty = 0;
                        (void)(*it).get_double().get(px);
                        if ((++it).error())
                            continue;
                        (void)(*it).get_double().get(qty);
                        dst.emplace_back(px, qty);
                    }
                }
            };

            {
                simdjson::ondemand::array bids_arr;
                if (data.find_field_unordered("bids").get_array().get(bids_arr))
                    return;
                parse_levels(std::move(bids_arr), book.bids);
            }
            {
                simdjson::ondemand::array asks_arr;
                if (data.find_field_unordered("asks").get_array().get(asks_arr))
                    return;
                parse_levels(std::move(asks_arr), book.asks);
            }

            if (!book.bids.empty() && !book.asks.empty()) {
                md::MdBbo bbo{recv_ns,
                              instrument_id,
                              book.bids[0].first,
                              book.bids[0].second,
                              book.asks[0].first,
                              book.asks[0].second};
                pub.publish(book);
                pub.publish(bbo);
            }
            return;
        }

        // --- Ticker (slow-cadence instrument stats: OI, volume, mark, index, last) ---
        // ticker.{symbol}.100ms carries per-instrument numeric fields that update
        // every few seconds at most. Bundling them into one InstrumentStatsUpdate
        // matches the source frame (atomic snapshot) and keeps the BBO firehose
        // free of slow-cadence fields. Each field is optional — if Deribit doesn't
        // provide it for this instrument type (spot has no OI, some non-perp tickers
        // omit mark/index), we leave it as NaN via the struct's default initialiser.
        if (channel.starts_with("ticker.")) {
            if (!on_instrument_stats)
                return;
            simdjson::ondemand::object data;
            if (data_val.get_object().get(data))
                return;

            messaging::InstrumentStatsUpdate stats;
            stats.instrument_id = instrument_id;
            stats.exchange_id = bpt::messages::ExchangeId::DERIBIT;
            stats.collected_ts_ns = recv_ns;

            // All fields are best-effort: simdjson .get() returns non-zero on
            // missing/wrong-type, leaving the destination untouched. Since the
            // struct's defaults are NaN, an absent field stays NaN in the
            // published message — consumers detect via std::isnan.
            (void)data.find_field_unordered("open_interest").get_double().get(stats.open_interest);
            (void)data.find_field_unordered("mark_price").get_double().get(stats.mark_price);
            (void)data.find_field_unordered("index_price").get_double().get(stats.index_price);
            (void)data.find_field_unordered("last_price").get_double().get(stats.last_price);

            // Deribit nests 24h volume under stats.volume (notional in base ccy
            // for futures/perps, or in contracts for options — units differ).
            simdjson::ondemand::object stats_obj;
            if (!data.find_field_unordered("stats").get_object().get(stats_obj))
                (void)stats_obj.find_field_unordered("volume").get_double().get(stats.volume_24h);

            on_instrument_stats(stats);

            // Funding rate (perps only): the ticker channel for *-PERPETUAL
            // instruments carries `funding_8h` — the 8-hour funding rate that
            // would apply at the next settlement tick if accrual stopped now.
            // Settlement is continuous but ticked at 00/08/16 UTC.
            // Options + futures don't have funding; their ticker omits the
            // field and the simdjson .get() leaves rate_8h at sentinel −1.
            if (on_funding_rate) {
                double rate_8h = -2.0;  // sentinel outside Deribit's expected ±0.05 range
                if (data.find_field_unordered("funding_8h").get_double().get(rate_8h) == simdjson::SUCCESS &&
                    rate_8h > -1.5) {
                    messaging::FundingRateUpdate fr;
                    fr.instrument_id = instrument_id;
                    fr.exchange_id = bpt::messages::ExchangeId::DERIBIT;
                    // rateBps stores rate × 1e6 (per FundingRate SBE field comment).
                    fr.rate_bps = static_cast<int32_t>(rate_8h * 1'000'000.0);
                    // Next funding ts: floor(now / 8h) + 8h, in nanoseconds.
                    constexpr uint64_t kEightHoursNs = 8ULL * 3600ULL * 1'000'000'000ULL;
                    fr.next_funding_ts_ns = ((recv_ns / kEightHoursNs) + 1) * kEightHoursNs;
                    fr.collected_ts_ns = recv_ns;
                    on_funding_rate(fr);
                }
            }
            return;
        }
    }

    /// \brief Clear per-session order book state. Called by the adapter before each (re)connect.
    void reset() { last_change_id_.clear(); }

    /// \brief Returns true and clears the flag if decode() saw a test_request heartbeat.
    [[nodiscard]] bool take_test_request() noexcept {
        if (!test_request_pending_)
            return false;
        test_request_pending_ = false;
        return true;
    }

    /// \brief Remove gap-detection state for an unsubscribed instrument.
    void forget(const std::string& symbol) { last_change_id_.erase(symbol); }

    bpt::common::util::LatencyHistogram decode_lat_;

private:
    SubscriptionMap& subs_;  ///< non-const: requeue() is called on order book gaps
    std::unordered_map<std::string, uint64_t> last_change_id_;
    bool test_request_pending_{false};
    uint64_t tick_count_{0};
};

}  // namespace bpt::md_gateway::adapter
