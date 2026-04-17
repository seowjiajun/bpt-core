#include "md_gateway/adapter/binance/binance_parser.h"

#include <messages/TradeSide.h>

#include <yggdrasil/logging.h>
#include <yggdrasil/util/parse_double.h>
#include <yggdrasil/util/tsc_clock.h>

namespace bpt::md_gateway::adapter {

void BinanceParser::parse(std::string_view payload,
                          uint64_t recv_ns,
                          messaging::IMdPublisher& pub,
                          messaging::FundingRateCallback& /*on_funding_rate*/) {
    // Combined stream wrapper: {"stream":"<sym>@<type>","data":{...}}
    const uint64_t parse_start_ns = ygg::util::TscClock::now_mono_ns();
    pad(payload);

    simdjson::ondemand::document doc;
    if (json_parser_.iterate(padded_buf_.data(), payload.size(), padded_buf_.size()).get(doc))
        return;

    std::string_view stream_name;
    if (doc["stream"].get_string().get(stream_name))
        return;

    simdjson::ondemand::object data;
    if (doc.find_field_unordered("data").get_object().get(data))
        return;

    const auto at_pos = stream_name.find('@');
    if (at_pos == std::string_view::npos)
        return;

    std::string_view sym = stream_name.substr(0, at_pos);
    std::string_view type = stream_name.substr(at_pos + 1);

    uint64_t instrument_id = subs_.find_id(sym);
    if (!instrument_id)
        return;

    if (type == "bookTicker") {
        // {"b":"<bid_px>","B":"<bid_qty>","a":"<ask_px>","A":"<ask_qty>",...}
        md::MdBbo bbo;
        bbo.timestamp_ns = recv_ns;
        bbo.instrument_id = instrument_id;
        if (ygg::util::ff_double(data["b"], bbo.bid_price))
            return;
        if (ygg::util::ff_double(data.find_field_unordered("B"), bbo.bid_qty))
            return;
        if (ygg::util::ff_double(data.find_field_unordered("a"), bbo.ask_price))
            return;
        if (ygg::util::ff_double(data.find_field_unordered("A"), bbo.ask_qty))
            return;

        uint64_t lat_ns = ygg::util::TscClock::now_mono_ns() - parse_start_ns;
        decode_lat_.record(lat_ns);
        if (++tick_count_ <= 20 || tick_count_ % 500 == 0)
            ygg::log::info("Binance BBO decode: {}ns tick={}", lat_ns, tick_count_);
        pub.publish(bbo);

    } else if (type == "aggTrade") {
        // {"p":"<price>","q":"<qty>","m":<bool>,...}
        // m == true: maker is buyer → aggressor is seller
        md::MdTrade trade;
        trade.timestamp_ns = recv_ns;
        trade.instrument_id = instrument_id;
        if (ygg::util::ff_double(data["p"], trade.price))
            return;
        if (ygg::util::ff_double(data.find_field_unordered("q"), trade.qty))
            return;

        bool maker_is_buyer = false;
        if (data.find_field_unordered("m").get_bool().get(maker_is_buyer))
            return;

        trade.side = maker_is_buyer ? bpt::messages::TradeSide::SELL : bpt::messages::TradeSide::BUY;
        pub.publish(trade);
    }
}

}  // namespace bpt::md_gateway::adapter
