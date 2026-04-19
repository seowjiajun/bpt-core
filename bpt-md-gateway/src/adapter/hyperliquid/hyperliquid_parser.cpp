#include "md_gateway/adapter/hyperliquid/hyperliquid_parser.h"

#include <messages/ExchangeId.h>
#include <messages/TradeSide.h>

#include <cmath>
#include <bpt_common/logging.h>
#include <bpt_common/util/parse_double.h>
#include <bpt_common/util/tsc_clock.h>

namespace bpt::md_gateway::adapter {

void HyperliquidParser::parse(std::string_view payload,
                              uint64_t recv_ns,
                              messaging::IMdPublisher& pub,
                              messaging::FundingRateCallback& on_funding_rate) {
    const uint64_t parse_start_ns = bpt::common::util::TscClock::now_mono_ns();
    pad(payload);

    simdjson::ondemand::document doc;
    if (json_parser_.iterate(padded_buf_.data(), payload.size(), padded_buf_.size()).get(doc))
        return;

    std::string_view channel;
    if (doc["channel"].get_string().get(channel))
        return;

    simdjson::ondemand::value data_val;
    if (doc.find_field_unordered("data").get(data_val))
        return;

    // --- BBO ---
    if (channel == "l2Book") {
        simdjson::ondemand::object book;
        if (data_val.get_object().get(book))
            return;

        std::string_view coin;
        if (book["coin"].get_string().get(coin))
            return;

        uint64_t instrument_id = subs_.find_id(coin);
        if (!instrument_id)
            return;

        simdjson::ondemand::array levels_outer;
        if (book.find_field_unordered("levels").get_array().get(levels_outer))
            return;

        md::MdBbo bbo;
        bbo.timestamp_ns = recv_ns;
        bbo.instrument_id = instrument_id;

        uint8_t side_idx = 0;
        for (auto side_res : levels_outer) {
            if (side_idx > 1)
                break;
            simdjson::ondemand::array side_arr;
            if (side_res.get_array().get(side_arr)) {
                ++side_idx;
                continue;
            }
            simdjson::ondemand::object lvl;
            if (side_arr.at(0).get_object().get(lvl)) {
                ++side_idx;
                continue;
            }
            double& px = (side_idx == 0) ? bbo.bid_price : bbo.ask_price;
            double& qty = (side_idx == 0) ? bbo.bid_qty : bbo.ask_qty;
            (void)bpt::common::util::ff_double(lvl["px"], px);
            (void)bpt::common::util::ff_double(lvl.find_field_unordered("sz"), qty);
            ++side_idx;
        }

        if (side_idx < 2)
            return;

        uint64_t lat_ns = bpt::common::util::TscClock::now_mono_ns() - parse_start_ns;
        decode_lat_.record(lat_ns);
        if (++tick_count_ <= 20 || tick_count_ % 500 == 0)
            bpt::common::log::info("Hyperliquid BBO decode: {}ns tick={}", lat_ns, tick_count_);
        pub.publish(bbo);

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

}  // namespace bpt::md_gateway::adapter
