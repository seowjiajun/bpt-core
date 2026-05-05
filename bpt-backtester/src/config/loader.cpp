#include "backtester/config/settings.h"

#include <toml++/toml.hpp>
#include <bpt_app/base_settings.h>

namespace bpt::backtester::config {

Settings load(const std::string& path) {
    toml::table root = toml::parse_file(path);

    Settings s;
    bpt::app::load_base_settings(root, s.base);

    if (auto* sim = root["simulation"].as_table()) {
        if (auto v = (*sim)["start"].value<std::string>())
            s.simulation.start = *v;
        if (auto v = (*sim)["end"].value<std::string>())
            s.simulation.end = *v;
        if (auto v = (*sim)["allow_partial_data"].value<bool>())
            s.simulation.allow_partial_data = *v;
        if (auto v = (*sim)["subscriber_wait_timeout_s"].value<int64_t>())
            s.simulation.subscriber_wait_timeout_s = static_cast<uint32_t>(*v);

        if (auto* lat = (*sim)["latency"].as_table()) {
            if (auto v = (*lat)["cex_base_ms"].value<int64_t>())
                s.simulation.latency.cex_base_ms = static_cast<uint32_t>(*v);
            if (auto v = (*lat)["hyperliquid_base_ms"].value<int64_t>())
                s.simulation.latency.hyperliquid_base_ms = static_cast<uint32_t>(*v);
            if (auto v = (*lat)["hyperliquid_jitter_ms"].value<int64_t>())
                s.simulation.latency.hyperliquid_jitter_ms = static_cast<uint32_t>(*v);
        }
    }

    if (auto* d = root["data"].as_table()) {
        if (auto v = (*d)["local_cache"].value<std::string>())
            s.data.local_cache = *v;
        if (auto v = (*d)["hyperliquid_refdata_snapshot"].value<std::string>())
            s.data.hyperliquid_refdata_snapshot = *v;
    }

    if (auto* ep = root["endpoints"].as_table()) {
        if (auto v = (*ep)["binance_md_port"].value<int64_t>())
            s.endpoints.binance_md_port = static_cast<uint16_t>(*v);
        if (auto v = (*ep)["okx_md_port"].value<int64_t>())
            s.endpoints.okx_md_port = static_cast<uint16_t>(*v);
        if (auto v = (*ep)["hyperliquid_md_port"].value<int64_t>())
            s.endpoints.hyperliquid_md_port = static_cast<uint16_t>(*v);
        if (auto v = (*ep)["deribit_md_port"].value<int64_t>())
            s.endpoints.deribit_md_port = static_cast<uint16_t>(*v);
        if (auto v = (*ep)["binance_order_port"].value<int64_t>())
            s.endpoints.binance_order_port = static_cast<uint16_t>(*v);
        if (auto v = (*ep)["okx_order_port"].value<int64_t>())
            s.endpoints.okx_order_port = static_cast<uint16_t>(*v);
        if (auto v = (*ep)["hyperliquid_order_port"].value<int64_t>())
            s.endpoints.hyperliquid_order_port = static_cast<uint16_t>(*v);
        if (auto v = (*ep)["deribit_order_port"].value<int64_t>())
            s.endpoints.deribit_order_port = static_cast<uint16_t>(*v);
        if (auto v = (*ep)["hyperliquid_info_port"].value<int64_t>())
            s.endpoints.hyperliquid_info_port = static_cast<uint16_t>(*v);
    }

    if (auto* arr = root["instruments"].as_array()) {
        for (auto& elem : *arr) {
            auto* t = elem.as_table();
            if (!t)
                continue;
            InstrumentConfig ic;
            if (auto v = (*t)["exchange"].value<std::string>())
                ic.exchange = *v;
            if (auto v = (*t)["symbol"].value<std::string>())
                ic.symbol = *v;
            if (!ic.exchange.empty() && !ic.symbol.empty())
                s.instruments.push_back(std::move(ic));
        }
    }

    if (auto* r = root["results"].as_table()) {
        if (auto v = (*r)["output_dir"].value<std::string>())
            s.results.output_dir = *v;
        if (auto v = (*r)["starting_capital"].value<double>())
            s.results.starting_capital = *v;
        if (auto v = (*r)["fee_bps_per_fill"].value<double>())
            s.results.fee_bps_per_fill = *v;
    }

    if (auto* a = root["aeron"].as_table()) {
        if (auto* t = (*a)["backtest_control"].as_table()) {
            if (auto v = (*t)["channel"].value<std::string>())
                s.aeron.backtest_control.channel = *v;
            if (auto v = (*t)["stream_id"].value<int64_t>())
                s.aeron.backtest_control.stream_id = static_cast<int32_t>(*v);
        }
        if (auto* t = (*a)["backtest_ack"].as_table()) {
            if (auto v = (*t)["channel"].value<std::string>())
                s.aeron.backtest_ack.channel = *v;
            if (auto v = (*t)["stream_id"].value<int64_t>())
                s.aeron.backtest_ack.stream_id = static_cast<int32_t>(*v);
        }
    }

    return s;
}

}  // namespace bpt::backtester::config
