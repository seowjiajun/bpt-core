#include "strategy/config/config.h"

#include <bpt_app/base_settings.h>
#include <bpt_common/aeron/streams_map.h>
#include <bpt_common/config/profile_config.h>
#include <bpt_common/logging.h>
#include <filesystem>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <stdexcept>
#include <toml++/toml.hpp>

namespace bpt::strategy {
namespace config {

AppConfig AppConfig::load(const std::string& path) {
    AppConfig app_cfg;

    toml::table cfg;
    try {
        cfg = toml::parse_file(path);
    } catch (const toml::parse_error& e) {
        throw std::runtime_error(fmt::format("Failed to load config {}: {}", path, std::string(e.description())));
    }

    // Profile sets `environment` so the strategy logs the same env as
    // refdata/md/og. Strategy keeps its own `md_exchanges` / `configured_exchanges`
    // (intentionally per-strategy — see [strategy] block — so a strategy
    // can subscribe to MD from venues it doesn't trade on for cross-venue signal).
    if (auto v = cfg["profile_config"].value<std::string>()) {
        auto profile = bpt::common::config::load_profile_config(*v);
        bpt::common::log::info("Loaded deployment profile from {} (env={}, exchanges=[{}])",
                               *v,
                               bpt::common::to_string(profile.environment),
                               fmt::join(profile.exchanges, ", "));
        if (!cfg.contains("environment"))
            cfg.insert("environment", std::string(bpt::common::to_string(profile.environment)));
    }

    bpt::app::load_base_settings(cfg, app_cfg.base);

    bpt::common::config::AeronStreamMap shared_streams;
    if (auto v = cfg["aeron_config"].value<std::string>()) {
        shared_streams = bpt::common::config::load_shared_streams(*v);
        bpt::common::log::info("Loaded shared aeron stream map from {} ({} streams)",
                               *v,
                               shared_streams.stream_ids.size());
        if (!shared_streams.media_driver_dir.empty())
            app_cfg.base.media_driver_dir = shared_streams.media_driver_dir;
    }

    using bpt::common::config::resolve_stream;
    // resolve_stream keys are the registry's dotted global names
    // (producer.stream); the assignment targets are this service's
    // per-consumer projection. These axes can differ: funding arrives via
    // strategy's refdata client (so the field sits under .refdata) but its
    // global name is md.funding_rate — md-gateway produces it.
    app_cfg.aeron.refdata.control = resolve_stream(shared_streams, "refdata.control", 1003);
    app_cfg.aeron.refdata.snapshot = resolve_stream(shared_streams, "refdata.snapshot", 1001);
    app_cfg.aeron.refdata.delta = resolve_stream(shared_streams, "refdata.delta", 1002);
    app_cfg.aeron.refdata.fee_schedule = resolve_stream(shared_streams, "refdata.fee_schedule", 1004);
    app_cfg.aeron.refdata.funding_rate = resolve_stream(shared_streams, "md.funding_rate", 2005);
    app_cfg.aeron.refdata.status = resolve_stream(shared_streams, "refdata.status", 1006);
    app_cfg.aeron.md.control = resolve_stream(shared_streams, "md.control", 0);
    app_cfg.aeron.md.data = resolve_stream(shared_streams, "md.feed", 0);
    app_cfg.aeron.md.ack_hb = resolve_stream(shared_streams, "md.ack_hb", 0);
    app_cfg.aeron.order.submit = resolve_stream(shared_streams, "order.submit", 0);
    app_cfg.aeron.order.exec_report = resolve_stream(shared_streams, "order.exec_report", 0);
    app_cfg.aeron.order.heartbeat = resolve_stream(shared_streams, "order.heartbeat", 0);
    app_cfg.aeron.order.account_snapshot = resolve_stream(shared_streams, "order.account_snapshot", 0);
    app_cfg.aeron.vol.surface = resolve_stream(shared_streams, "pricer.vol_surface", 0);
    app_cfg.aeron.vol.pricer_status = resolve_stream(shared_streams, "pricer.status", 0);
    app_cfg.aeron.toxicity = resolve_stream(shared_streams, "analytics.toxicity", 0);
    app_cfg.aeron.backtest.control = resolve_stream(shared_streams, "backtest.control", 9002);
    app_cfg.aeron.backtest.ack = resolve_stream(shared_streams, "backtest.ack", 9001);
    app_cfg.aeron.console_control = resolve_stream(shared_streams, "bridge.console_control", 9003);
    app_cfg.aeron.portfolio = resolve_stream(shared_streams, "bridge.portfolio", 9004);

    // [strategy] in the parent config carries service-level fields (correlation_id).
    // The strategy body — type / params / risk / etc. — lives either in the same
    // [strategy] table or, when strategy_config is set, in a separate file whose
    // root is [strategy]. Service-level and body fields share the table name; the
    // parent's correlation_id is read here, the body is resolved below.
    const auto* svc = cfg["strategy"].as_table();
    if (!svc)
        throw std::runtime_error("Missing 'strategy' block in config");

    app_cfg.strat.correlation_id = static_cast<uint64_t>((*svc)["correlation_id"].value<int64_t>().value_or(2001));

    // If strategy_config is set, load [strategy] from that file.
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
    const auto* s = (*strategy_root)["strategy"].as_table();
    if (!s)
        throw std::runtime_error("Missing 'strategy' block in strategy config");

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
        sc.schedule.refdata_heartbeat_timeout_ns =
            static_cast<uint64_t>((*sch)["refdata_heartbeat_timeout_ns"].value<int64_t>().value_or(25'000'000'000LL));
        sc.schedule.startup_refdata_timeout_ns =
            static_cast<uint64_t>((*sch)["startup_refdata_timeout_ns"].value<int64_t>().value_or(60'000'000'000LL));

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

    if (auto* ws = (*s)["warm_start"].as_table()) {
        sc.warm_start.state_dir = (*ws)["state_dir"].value<std::string>().value_or("");
        sc.warm_start.max_age_s = static_cast<uint64_t>((*ws)["max_age_s"].value<int64_t>().value_or(600));
    }

    if (auto* p = (*s)["params"].as_table())
        sc.params = *p;

    if (auto v = cfg["backtest_mode"].value<bool>())
        app_cfg.backtest_mode = *v;

    return app_cfg;
}

}  // namespace config
}  // namespace bpt::strategy
