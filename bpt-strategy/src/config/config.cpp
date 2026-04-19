#include "strategy/config/config.h"

#include <filesystem>
#include <fmt/format.h>
#include <stdexcept>
#include <toml++/toml.hpp>
#include <bpt_app/base_settings.h>

namespace bpt::strategy {
namespace config {

namespace {

bpt::common::config::StreamConfig load_stream(const toml::table* t, std::string default_channel, int32_t default_stream_id) {
    bpt::common::config::StreamConfig s{std::move(default_channel), default_stream_id};
    if (!t)
        return s;
    if (auto v = (*t)["channel"].value<std::string>())
        s.channel = *v;
    if (auto v = (*t)["stream_id"].value<int64_t>())
        s.stream_id = static_cast<int32_t>(*v);
    return s;
}

}  // namespace

AppConfig AppConfig::load(const std::string& path) {
    AppConfig app_cfg;

    toml::table cfg;
    try {
        cfg = toml::parse_file(path);
    } catch (const toml::parse_error& e) {
        throw std::runtime_error(fmt::format("Failed to load config {}: {}", path, std::string(e.description())));
    }

    bpt::app::load_base_settings(cfg, app_cfg.base);

    const auto* a = cfg["aeron"].as_table();
    if (!a)
        throw std::runtime_error("Missing 'aeron' block in config");

    app_cfg.aeron.refdata_control = load_stream((*a)["refdata_control"].as_table(), "aeron:ipc", 1003);
    app_cfg.aeron.refdata_snapshot = load_stream((*a)["refdata_snapshot"].as_table(), "aeron:ipc", 1001);
    app_cfg.aeron.refdata_delta = load_stream((*a)["refdata_delta"].as_table(), "aeron:ipc", 1002);
    app_cfg.aeron.fee_schedule = load_stream((*a)["fee_schedule"].as_table(), "aeron:ipc", 1004);
    app_cfg.aeron.funding_rate = load_stream((*a)["funding_rate"].as_table(), "aeron:ipc", 1005);
    app_cfg.aeron.refdata_status = load_stream((*a)["refdata_status"].as_table(), "aeron:ipc", 1006);
    app_cfg.aeron.md_control = load_stream((*a)["md_control"].as_table(), "aeron:ipc", 0);
    app_cfg.aeron.md_data = load_stream((*a)["md_data"].as_table(), "aeron:ipc", 0);
    app_cfg.aeron.md_ack_hb = load_stream((*a)["md_ack_hb"].as_table(), "aeron:ipc", 0);
    app_cfg.aeron.order = load_stream((*a)["order"].as_table(), "aeron:ipc", 0);
    app_cfg.aeron.exec_report = load_stream((*a)["exec_report"].as_table(), "aeron:ipc", 0);
    app_cfg.aeron.heartbeat = load_stream((*a)["heartbeat"].as_table(), "aeron:ipc", 0);
    app_cfg.aeron.account_snapshot = load_stream((*a)["account_snapshot"].as_table(), "aeron:ipc", 0);
    app_cfg.aeron.vol_surface = load_stream((*a)["vol_surface"].as_table(), "aeron:ipc", 0);
    app_cfg.aeron.pricer_status = load_stream((*a)["pricer_status"].as_table(), "aeron:ipc", 0);
    app_cfg.aeron.toxicity = load_stream((*a)["toxicity"].as_table(), "aeron:ipc", 0);
    app_cfg.aeron.backtest_control = load_stream((*a)["backtest_control"].as_table(), "aeron:ipc", 9002);
    app_cfg.aeron.backtest_ack = load_stream((*a)["backtest_ack"].as_table(), "aeron:ipc", 9001);
    app_cfg.aeron.dashboard_control = load_stream((*a)["dashboard_control"].as_table(), "aeron:ipc", 9003);
    app_cfg.aeron.dashboard_snapshot = load_stream((*a)["dashboard_snapshot"].as_table(), "aeron:ipc", 9004);

    const auto* f = cfg["fenrir"].as_table();
    if (!f)
        throw std::runtime_error("Missing 'fenrir' block in config");

    app_cfg.strat.correlation_id = static_cast<uint64_t>((*f)["correlation_id"].value<int64_t>().value_or(2001));

    // If strategy_config is set, load [fenrir.strategy] from that file.
    // Path is resolved relative to the parent config's directory.
    toml::table strategy_file;
    if (auto sc_path = cfg["strategy_config"].value<std::string>()) {
        auto resolved = (std::filesystem::path(path).parent_path() / *sc_path).string();
        try {
            strategy_file = toml::parse_file(resolved);
        } catch (const toml::parse_error& e) {
            throw std::runtime_error(
                fmt::format("Failed to load strategy_config {}: {}", resolved, std::string(e.description())));
        }
    }

    const toml::table* strategy_root = strategy_file.empty() ? &cfg : &strategy_file;
    const auto* strategy_fenrir = (*strategy_root)["fenrir"].as_table();
    const auto* s = strategy_fenrir ? (*strategy_fenrir)["strategy"].as_table() : (*f)["strategy"].as_table();
    if (!s)
        throw std::runtime_error("Missing 'fenrir.strategy' block in config");

    auto& sc = app_cfg.strat.strategy;
    sc.type = (*s)["type"].value<std::string>().value_or("");
    sc.enabled = (*s)["enabled"].value<bool>().value_or(true);

    if (auto* arr = (*s)["instruments"].as_array())
        for (auto& elem : *arr)
            if (auto v = elem.value<std::string>())
                sc.instruments.push_back(*v);

    if (auto* arr = (*s)["md_exchanges"].as_array())
        for (auto& elem : *arr)
            if (auto v = elem.value<std::string>())
                sc.md_exchanges.push_back(*v);

    if (auto* ve = (*s)["venue_exec"].as_table()) {
        for (auto& [venue_key, v_node] : *ve) {
            const auto* v = v_node.as_table();
            if (!v)
                continue;
            VenueExecConfig vc;
            vc.enabled = (*v)["enabled"].value<bool>().value_or(true);
            vc.latency_budget_us = static_cast<uint64_t>((*v)["latency_budget_us"].value<int64_t>().value_or(1000));
            vc.order_type = (*v)["order_type"].value<std::string>().value_or("LIMIT");
            vc.tif = (*v)["tif"].value<std::string>().value_or("GTC");
            vc.max_retries = static_cast<uint32_t>((*v)["max_retries"].value<int64_t>().value_or(3));
            sc.venue_exec[std::string(venue_key)] = vc;
        }
    }

    if (auto* r = (*s)["risk"].as_table()) {
        sc.risk.max_position_usd = (*r)["max_position_usd"].value<double>().value_or(10000.0);
        sc.risk.max_order_size_usd = (*r)["max_order_size_usd"].value<double>().value_or(1000.0);
        sc.risk.max_open_orders = static_cast<uint32_t>((*r)["max_open_orders"].value<int64_t>().value_or(10));
        // max_daily_loss_usd intentionally NOT parsed here — enforcement
        // lives in order-gateway (see comment in RiskConfig). The TOML
        // field is tolerated but ignored; toml++ silently skips unknown
        // keys so legacy configs still load.
    }

    if (auto* sch = (*s)["schedule"].as_table()) {
        sc.schedule.require_refdata_ready = (*sch)["require_refdata_ready"].value<bool>().value_or(true);
        sc.schedule.md_staleness_threshold_ms =
            static_cast<uint64_t>((*sch)["md_staleness_threshold_ms"].value<int64_t>().value_or(5000));
        sc.schedule.max_refdata_staleness_ns =
            static_cast<uint64_t>((*sch)["max_refdata_staleness_ns"].value<int64_t>().value_or(300'000'000'000LL));

        if (auto* arr = (*sch)["configured_exchanges"].as_array())
            for (auto& elem : *arr)
                if (auto v = elem.value<std::string>())
                    sc.schedule.configured_exchanges.push_back(*v);

        uint8_t mask = 0;
        for (const auto& ex : sc.schedule.configured_exchanges) {
            if (ex == "BINANCE")
                mask |= 0x01;
            else if (ex == "OKX")
                mask |= 0x02;
            else if (ex == "HYPERLIQUID")
                mask |= 0x04;
            else if (ex == "DERIBIT")
                mask |= 0x08;
        }
        sc.schedule.configured_exchanges_mask = mask;
    }

    if (auto* p = (*s)["params"].as_table())
        sc.params = *p;

    if (auto v = cfg["backtest_mode"].value<bool>())
        app_cfg.backtest_mode = *v;

    return app_cfg;
}

}  // namespace config
}  // namespace bpt::strategy
