#include "backtester/config/settings.h"

#include "backtester/calendar/session_calendar.h"

#include <algorithm>
#include <stdexcept>
#include <toml++/toml.hpp>
#include <bpt_app/base_settings.h>
#include <bpt_common/logging.h>

namespace bpt::backtester::config {

namespace {

VenueLatencySpec parse_venue_spec(const toml::table& t) {
    VenueLatencySpec s{};
    if (auto v = t["submit_to_match_base_ns"].value<int64_t>())
        s.submit_to_match_base_ns = static_cast<uint64_t>(*v);
    if (auto v = t["submit_to_match_jitter_ns"].value<int64_t>())
        s.submit_to_match_jitter_ns = static_cast<uint64_t>(*v);
    if (auto v = t["match_to_report_base_ns"].value<int64_t>())
        s.match_to_report_base_ns = static_cast<uint64_t>(*v);
    if (auto v = t["match_to_report_jitter_ns"].value<int64_t>())
        s.match_to_report_jitter_ns = static_cast<uint64_t>(*v);
    return s;
}

}  // namespace

Settings load(const std::string& path) {
    toml::table root = toml::parse_file(path);

    Settings s;
    bpt::app::load_base_settings(root, s.base);

    if (auto* sim = root["simulation"].as_table()) {
        auto top_start  = (*sim)["start"].value<std::string>();
        auto top_end    = (*sim)["end"].value<std::string>();
        auto* win_arr   = (*sim)["windows"].as_array();
        auto* sess_arr  = (*sim)["sessions"].as_array();

        const bool has_top  = top_start.has_value() || top_end.has_value();
        const bool has_arr  = win_arr != nullptr;
        const bool has_sess = sess_arr != nullptr;

        const int set_count = (has_top ? 1 : 0) + (has_arr ? 1 : 0) + (has_sess ? 1 : 0);
        if (set_count > 1)
            throw std::runtime_error(
                "simulation: pick exactly one of top-level start/end, "
                "[[simulation.windows]], or [[simulation.sessions]]");

        if (has_arr) {
            for (auto& elem : *win_arr) {
                auto* t = elem.as_table();
                if (!t)
                    continue;
                TimeWindow w;
                if (auto v = (*t)["start"].value<std::string>()) w.start = *v;
                if (auto v = (*t)["end"].value<std::string>())   w.end   = *v;
                if (w.start.empty() || w.end.empty())
                    throw std::runtime_error(
                        "simulation.windows: every entry must have non-empty start and end");
                s.simulation.windows.push_back(std::move(w));
            }
            if (s.simulation.windows.empty())
                throw std::runtime_error("simulation.windows: array is empty");
        } else if (has_sess) {
            const auto cal = bpt::backtester::calendar::SessionCalendar::with_crypto_defaults();
            for (auto& elem : *sess_arr) {
                auto* t = elem.as_table();
                if (!t)
                    continue;
                auto name = (*t)["name"].value<std::string>();
                auto* dates = (*t)["dates"].as_array();
                if (!name || !dates)
                    throw std::runtime_error(
                        "simulation.sessions: each entry needs name and dates[]");
                std::vector<std::string> dlist;
                for (auto& dv : *dates)
                    if (auto sv = dv.value<std::string>())
                        dlist.push_back(*sv);
                for (const auto& w : cal.resolve(*name, dlist))
                    s.simulation.windows.push_back(TimeWindow{w.start, w.end});
            }
            if (s.simulation.windows.empty())
                throw std::runtime_error("simulation.sessions: produced no windows");
        } else if (top_start && top_end) {
            s.simulation.windows.push_back(TimeWindow{*top_start, *top_end});
        } else {
            throw std::runtime_error(
                "simulation: must specify top-level start/end, "
                "[[simulation.windows]], or [[simulation.sessions]]");
        }

        // Sort by start so .start / .end (back-compat scalars) describe the
        // span correctly. Sorting is stable so user-supplied order is
        // preserved between equal starts.
        std::stable_sort(s.simulation.windows.begin(), s.simulation.windows.end(),
                         [](const TimeWindow& a, const TimeWindow& b) {
                             return a.start < b.start;
                         });
        s.simulation.start = s.simulation.windows.front().start;
        s.simulation.end   = s.simulation.windows.back().end;

        if (auto v = (*sim)["allow_partial_data"].value<bool>())
            s.simulation.allow_partial_data = *v;
        if (auto v = (*sim)["subscriber_wait_timeout_s"].value<int64_t>())
            s.simulation.subscriber_wait_timeout_s = static_cast<uint32_t>(*v);

        if (auto* lat = (*sim)["latency"].as_table()) {
            if (auto v = (*lat)["seed"].value<int64_t>())
                s.simulation.latency.seed = static_cast<uint64_t>(*v);

            if (auto* d = (*lat)["default"].as_table())
                s.simulation.latency.default_spec = parse_venue_spec(*d);

            for (const char* venue : {"BINANCE", "OKX", "HYPERLIQUID", "DERIBIT"}) {
                if (auto* vt = (*lat)[venue].as_table())
                    s.simulation.latency.per_venue[venue] = parse_venue_spec(*vt);
            }

            // Back-compat for cex_base_ms / hyperliquid_base_ms / hyperliquid_jitter_ms.
            // Pre-Phase-3 these fields existed but no consumer ever read them; users
            // expecting latency to be applied got zero. We translate them onto the
            // new per-venue submit_to_match leg so configs that *had* these set
            // start actually feeling the latency they always thought they had.
            // match_to_report stays at zero from the legacy form — the legacy
            // schema didn't expose that leg.
            bool legacy_used = false;
            auto fill_if_unset = [&](const char* venue, uint64_t base_ns, uint64_t jitter_ns) {
                auto& spec = s.simulation.latency.per_venue[venue];
                if (spec.submit_to_match_base_ns == 0 && base_ns > 0)
                    spec.submit_to_match_base_ns = base_ns;
                if (spec.submit_to_match_jitter_ns == 0 && jitter_ns > 0)
                    spec.submit_to_match_jitter_ns = jitter_ns;
            };
            if (auto v = (*lat)["cex_base_ms"].value<int64_t>()) {
                legacy_used = true;
                const uint64_t ns = static_cast<uint64_t>(*v) * 1'000'000ULL;
                fill_if_unset("BINANCE", ns, 0);
                fill_if_unset("OKX", ns, 0);
                fill_if_unset("DERIBIT", ns, 0);
            }
            if (auto v = (*lat)["hyperliquid_base_ms"].value<int64_t>()) {
                legacy_used = true;
                fill_if_unset("HYPERLIQUID", static_cast<uint64_t>(*v) * 1'000'000ULL, 0);
            }
            if (auto v = (*lat)["hyperliquid_jitter_ms"].value<int64_t>()) {
                legacy_used = true;
                fill_if_unset("HYPERLIQUID", 0, static_cast<uint64_t>(*v) * 1'000'000ULL);
            }
            if (legacy_used) {
                bpt::common::log::warn(
                    "[config] simulation.latency: legacy ms fields are deprecated. "
                    "Use [simulation.latency.<VENUE>] submit_to_match_base_ns / _jitter_ns / "
                    "match_to_report_base_ns / _jitter_ns instead.");
            }
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

        // Per-venue fee table. Format:
        //   [results.fees.OKX] maker_bps = 2  taker_bps = 5
        //   [results.fees.HYPERLIQUID] maker_bps = -1.5  taker_bps = 4.5
        if (auto* fees = (*r)["fees"].as_table()) {
            for (const auto& [venue, val] : *fees) {
                auto* vt = val.as_table();
                if (!vt)
                    continue;
                ResultsConfig::FeeRates rates;
                if (auto v = (*vt)["maker_bps"].value<double>())
                    rates.maker_bps = *v;
                if (auto v = (*vt)["taker_bps"].value<double>())
                    rates.taker_bps = *v;
                s.results.fees_by_venue[std::string{venue.str()}] = rates;
            }
        }

        // Back-compat: old `fee_bps_per_fill` scalar is treated as both
        // maker and taker, applied to every venue in the run. Loud
        // warning so configs get migrated.
        if (auto v = (*r)["fee_bps_per_fill"].value<double>()) {
            for (const auto& inst : s.instruments) {
                auto& rates = s.results.fees_by_venue[inst.exchange];
                rates.maker_bps = *v;
                rates.taker_bps = *v;
            }
        }
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
