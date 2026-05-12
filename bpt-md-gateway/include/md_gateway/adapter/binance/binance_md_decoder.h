#pragma once

/// \file
/// \brief Binance MD frame decoder (JSON → SBE).

#include "md_gateway/adapter/common/json_decoder_base.h"
#include "md_gateway/adapter/common/subscription_map.h"
#include "md_gateway/md/md_types.h"
#include "md_gateway/messaging/funding_rate_publisher.h"

#include <messages/TradeSide.h>

#include <bpt_common/logging.h>
#include <bpt_common/util/latency_histogram.h>
#include <bpt_common/util/parse_double.h>
#include <bpt_common/util/tsc_clock.h>
#include <cstdint>
#include <string_view>

namespace bpt::md_gateway::adapter {

/// \brief Decodes Binance combined-stream WS frames and publishes SBE.
///
/// Templated on Pub for vtable-free publish().
///
/// Handled message types:
///   - `<sym>@bookTicker` → publish_bbo
///   - `<sym>@aggTrade`   → publish_trade
///
/// Funding rates arrive on a separate BinanceMdAdapter thread
/// (fstream.binance.com) and are not handled here.
template <class Pub>
class BinanceMdDecoder : public JsonDecoderBase {
public:
    explicit BinanceMdDecoder(const SubscriptionMap& subs) : subs_(subs) {}

    void decode(std::string_view payload,
                uint64_t recv_ns,
                Pub& pub,
                messaging::FundingRateCallback& /*on_funding_rate*/) {
        // Combined stream wrapper: {"stream":"<sym>@<type>","data":{...}}
        const uint64_t parse_start_ns = bpt::common::util::TscClock::now_mono_ns();
        pad(payload);

        simdjson::ondemand::document doc;
        if (json_parser_.iterate(padded_buf_.data(), payload.size(), padded_buf_.size()).get(doc)) [[unlikely]]
            return;

        std::string_view stream_name;
        if (doc["stream"].get_string().get(stream_name)) [[unlikely]]
            return;

        simdjson::ondemand::object data;
        if (doc.find_field_unordered("data").get_object().get(data)) [[unlikely]]
            return;

        const auto at_pos = stream_name.find('@');
        if (at_pos == std::string_view::npos) [[unlikely]]
            return;

        std::string_view sym = stream_name.substr(0, at_pos);
        std::string_view type = stream_name.substr(at_pos + 1);

        uint64_t instrument_id = subs_.find_id(sym);
        if (!instrument_id) [[unlikely]]
            return;

        if (type == "bookTicker") {
            md::MdBbo bbo;
            bbo.timestamp_ns = recv_ns;
            bbo.instrument_id = instrument_id;
            if (bpt::common::util::ff_double(data["b"], bbo.bid_price)) [[unlikely]]
                return;
            if (bpt::common::util::ff_double(data.find_field_unordered("B"), bbo.bid_qty)) [[unlikely]]
                return;
            if (bpt::common::util::ff_double(data.find_field_unordered("a"), bbo.ask_price)) [[unlikely]]
                return;
            if (bpt::common::util::ff_double(data.find_field_unordered("A"), bbo.ask_qty)) [[unlikely]]
                return;

            uint64_t lat_ns = bpt::common::util::TscClock::now_mono_ns() - parse_start_ns;
            decode_lat_.record(lat_ns);
            if (++tick_count_ <= 20 || tick_count_ % 500 == 0)
                bpt::common::log::info("Binance BBO decode: {}ns tick={}", lat_ns, tick_count_);
            pub.publish(bbo);

        } else if (type == "aggTrade") {
            md::MdTrade trade;
            trade.timestamp_ns = recv_ns;
            trade.instrument_id = instrument_id;
            if (bpt::common::util::ff_double(data["p"], trade.price)) [[unlikely]]
                return;
            if (bpt::common::util::ff_double(data.find_field_unordered("q"), trade.qty)) [[unlikely]]
                return;

            bool maker_is_buyer = false;
            if (data.find_field_unordered("m").get_bool().get(maker_is_buyer)) [[unlikely]]
                return;

            // m == true: maker is buyer → aggressor is seller
            trade.side = maker_is_buyer ? bpt::messages::TradeSide::SELL : bpt::messages::TradeSide::BUY;
            pub.publish(trade);
        }
    }

    bpt::common::util::LatencyHistogram decode_lat_;

private:
    const SubscriptionMap& subs_;
    uint64_t tick_count_{0};
};

}  // namespace bpt::md_gateway::adapter
