// dl_smoke — loads Parquet via bpt-backtester's DataLoader and prints a
// summary of what it reads. Used to validate a freshly-recorded tape
// against backtester's consumer without spinning up the full 6-process
// backtest stack.
//
// Usage:
//   dl_smoke --exchange OKX --symbol BTC-USDT \
//            --start 2026-04-24 --end 2026-04-24 \
//            [--cache /opt/bpt/data/backtest-cache]

#include "backtester/config/settings.h"
#include "backtester/data/data_loader.h"

#include <CLI/CLI.hpp>
#include <cstdio>
#include <stdexcept>
#include <string>

using namespace bpt::backtester;

int main(int argc, char** argv) {
    CLI::App cli{"DataLoader smoke test"};
    std::string exchange, symbol, start_day, end_day;
    std::string cache = "/opt/bpt/data/backtest-cache";
    cli.add_option("--exchange", exchange, "e.g. OKX")->required();
    cli.add_option("--symbol", symbol, "e.g. BTC-USDT")->required();
    cli.add_option("--start", start_day, "YYYY-MM-DD")->required();
    cli.add_option("--end", end_day, "YYYY-MM-DD")->required();
    cli.add_option("--cache", cache, "Parquet root");
    CLI11_PARSE(cli, argc, argv);

    config::DataConfig data_cfg;
    data_cfg.local_cache = cache;

    config::SimulationConfig sim_cfg;
    sim_cfg.start = start_day + "T00:00:00Z";
    sim_cfg.end = end_day + "T23:59:59Z";
    sim_cfg.allow_partial_data = true;

    std::vector<config::InstrumentConfig> instruments{{exchange, symbol}};

    try {
        data::DataLoader loader(data_cfg, sim_cfg, instruments);
        loader.validate();

        uint64_t trade_count = 0, book_count = 0;
        uint64_t first_ts = 0, last_ts = 0;

        while (auto ev = loader.next()) {
            if (first_ts == 0)
                first_ts = ev->timestamp_ns;
            last_ts = ev->timestamp_ns;

            if (ev->type == data::MarketEvent::Type::TRADE)
                ++trade_count;
            else if (ev->type == data::MarketEvent::Type::ORDER_BOOK)
                ++book_count;
        }

        std::printf("DataLoader OK:\n");
        std::printf("  exchange=%s symbol=%s\n", exchange.c_str(), symbol.c_str());
        std::printf("  trades=%lu book=%lu total=%lu\n", trade_count, book_count, trade_count + book_count);
        if (first_ts != 0) {
            std::printf("  first_ts_ns=%lu\n", first_ts);
            std::printf("  last_ts_ns =%lu\n", last_ts);
            std::printf("  span_s     =%.1f\n", (last_ts - first_ts) / 1e9);
        } else {
            std::printf("  (no events read — check cache path and symbol)\n");
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "DataLoader FAILED: %s\n", e.what());
        return 1;
    }
}
