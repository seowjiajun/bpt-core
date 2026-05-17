#pragma once

/// \file
/// \brief Hyperliquid MD frame decoder (JSON → SBE).

#include "md_gateway/adapter/common/json_decoder_base.h"
#include "md_gateway/adapter/common/subscription_map.h"
#include "md_gateway/md/md_publisher_concept.h"
#include "md_gateway/md/md_types.h"
#include "md_gateway/messaging/publishers/api/funding_rate_publisher.h"
#include "md_gateway/messaging/publishers/api/instrument_stats_publisher.h"

#include <messages/ExchangeId.h>
#include <messages/TradeSide.h>

#include <bpt_common/logging.h>
#include <bpt_common/util/latency_histogram.h>
#include <bpt_common/util/parse_double.h>
#include <bpt_common/util/tsc_clock.h>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace bpt::md_gateway::adapter {

/// \brief Decodes Hyperliquid WS frames and publishes SBE.
///
/// Templated on Pub for vtable-free publish().
///
/// Handled channels:
///   - l2Book         → publish_bbo (top of book only)
///   - trades         → publish_trade (one publish per trade in the array)
///   - activeAssetCtx → on_funding_rate callback (no nextFundingTime from HL)
template <md::MdSink Pub>
class HyperliquidMdDecoder : public JsonDecoderBase {
public:
    explicit HyperliquidMdDecoder(const SubscriptionMap& subs) : subs_(subs) {}

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

        std::string_view channel;
        if (doc["channel"].get_string().get(channel)) [[unlikely]]
            return;

        simdjson::ondemand::value data_val;
        if (doc.find_field_unordered("data").get(data_val)) [[unlikely]]
            return;

        // --- BBO ---
        if (channel == "l2Book") {
            simdjson::ondemand::object book;
            if (data_val.get_object().get(book)) [[unlikely]]
                return;

            std::string_view coin;
            if (book["coin"].get_string().get(coin)) [[unlikely]]
                return;

            uint64_t instrument_id = subs_.find_id(coin);
            if (!instrument_id) [[unlikely]]
                return;

            simdjson::ondemand::array levels_outer;
            if (book.find_field_unordered("levels").get_array().get(levels_outer)) [[unlikely]]
                return;

            md::MdBbo bbo;
            bbo.timestamp_ns = recv_ns;
            bbo.instrument_id = instrument_id;

            // Per-subscription depth — drives whether to emit MdOrderBook
            // alongside MdBbo. Strategies with order_book_depth=0 only need
            // BBO; depth>0 strategies (and the deterministic backtest
            // harness) need the L2 ladder for queue-aware fill matching
            // and OFI features. HL's `l2Book` payload ships up to 20 levels
            // per side; cap at min(subscribed_depth, kMaxBookLevels).
            const uint8_t sub_depth = subs_.find_depth(instrument_id);
            const std::size_t depth_cap = std::min<std::size_t>(sub_depth > 0 ? sub_depth : 0, md::kMaxBookLevels);

            md::MdOrderBook out_book;
            out_book.timestamp_ns = recv_ns;
            out_book.instrument_id = instrument_id;

            uint8_t side_idx = 0;
            for (auto side_res : levels_outer) {
                if (side_idx > 1)
                    break;
                simdjson::ondemand::array side_arr;
                if (side_res.get_array().get(side_arr)) {
                    ++side_idx;
                    continue;
                }
                std::size_t level_idx = 0;
                for (auto lvl_res : side_arr) {
                    simdjson::ondemand::object lvl;
                    if (lvl_res.get_object().get(lvl))
                        break;
                    double px = 0.0, qty = 0.0;
                    (void)bpt::common::util::ff_double(lvl["px"], px);
                    (void)bpt::common::util::ff_double(lvl.find_field_unordered("sz"), qty);
                    if (level_idx == 0) {
                        // Top-of-book — populate MdBbo for BBO consumers.
                        if (side_idx == 0) {
                            bbo.bid_price = px;
                            bbo.bid_qty = qty;
                        } else {
                            bbo.ask_price = px;
                            bbo.ask_qty = qty;
                        }
                    }
                    if (depth_cap > 0 && level_idx < depth_cap) {
                        if (side_idx == 0)
                            out_book.bids.emplace_back(px, qty);
                        else
                            out_book.asks.emplace_back(px, qty);
                    }
                    ++level_idx;
                    if (depth_cap == 0 && level_idx >= 1)
                        break;  // BBO-only consumer — stop after [0]
                }
                ++side_idx;
            }

            if (side_idx < 2) [[unlikely]]
                return;

            uint64_t lat_ns = bpt::common::util::TscClock::now_mono_ns() - parse_start_ns;
            decode_lat_.record(lat_ns);
            if (++tick_count_ <= 20 || tick_count_ % 500 == 0)
                bpt::common::log::info("Hyperliquid BBO decode: {}ns tick={}", lat_ns, tick_count_);
            pub.publish(bbo);
            // Only publish MdOrderBook when at least one side had levels
            // captured at sub_depth. Production HL strategies with
            // order_book_depth=0 won't set sub_depth>0, so this branch is
            // a no-op for them — preserving today's wire-level behavior.
            if (depth_cap > 0 && (!out_book.bids.empty() || !out_book.asks.empty()))
                pub.publish(out_book);

            // --- Trades ---
        } else if (channel == "trades") {
            simdjson::ondemand::array trades;
            if (data_val.get_array().get(trades))
                return;

            for (auto trade_res : trades) {
                simdjson::ondemand::object obj;
                if (trade_res.get_object().get(obj))
                    continue;

                std::string_view coin;
                if (obj["coin"].get_string().get(coin))
                    continue;

                uint64_t instrument_id = subs_.find_id(coin);
                if (!instrument_id)
                    continue;

                md::MdTrade trade;
                trade.timestamp_ns = recv_ns;
                trade.instrument_id = instrument_id;
                (void)bpt::common::util::ff_double(obj.find_field_unordered("px"), trade.price);
                (void)bpt::common::util::ff_double(obj.find_field_unordered("sz"), trade.qty);

                std::string_view side_sv;
                obj.find_field_unordered("side").get_string().get(side_sv);
                trade.side = (side_sv == "B") ? bpt::messages::TradeSide::BUY : bpt::messages::TradeSide::SELL;
                pub.publish(trade);
            }

            // --- Funding rate ---
        } else if (channel == "activeAssetCtx") {
            if (!on_funding_rate)
                return;

            simdjson::ondemand::object data_obj;
            if (data_val.get_object().get(data_obj))
                return;

            std::string_view coin;
            if (data_obj["coin"].get_string().get(coin))
                return;

            uint64_t instrument_id = subs_.find_id(coin);
            if (!instrument_id)
                return;

            simdjson::ondemand::object ctx;
            if (data_obj.find_field_unordered("ctx").get_object().get(ctx))
                return;

            double rate = 0.0;
            (void)bpt::common::util::ff_double(ctx["funding"], rate);

            messaging::FundingRateUpdate fr;
            fr.instrument_id = instrument_id;
            fr.exchange_id = bpt::messages::ExchangeId::HYPERLIQUID;
            fr.rate_bps = static_cast<int32_t>(std::round(rate * 1'000'000.0));
            fr.next_funding_ts_ns = 0;
            fr.collected_ts_ns = recv_ns;
            on_funding_rate(fr);
        }
    }

    bpt::common::util::LatencyHistogram decode_lat_;

private:
    const SubscriptionMap& subs_;
    uint64_t tick_count_{0};
};

}  // namespace bpt::md_gateway::adapter
