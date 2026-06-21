/// bpt-canon-ingest-okx-trades — converts OKX's free historical trades
/// CSV (one day, one instrument family per file) into a `.canon` file.
///
/// Sibling producer to `bpt-canon-replay`. Same canonical event schema
/// downstream, different input format. The whole point of canon is
/// that consumers (backtester) can't tell which producer wrote a file;
/// only the file-header `producer_kind` records the provenance.
///
/// Input file layout (from OKX https://www.okx.com/en-sg/historical-data):
///
///   instrument_name,trade_id,side,price,size,created_time
///   BTC-USDT-SWAP,2562435589,buy,76416.3,0.02,1777564800028
///   ...
///
/// One file per (instrument-family, day) for SWAP/FUTURES, per instrument
/// for SPOT. `created_time` is wall-clock milliseconds since Unix epoch.
/// The file is sorted ascending by trade_id (which is monotonic with
/// time on OKX) but rows can repeat the same created_time when multiple
/// trades cross at the same millisecond — that's fine, canon allows
/// ts collisions.
///
/// Today: trades only. L2 orderbook ingestion (NDJSON snapshots/updates)
/// lives in a sibling binary, `bpt-canon-ingest-okx-l2`. The two streams
/// merge into one canon file via the merge stage of the build.

#include "canon/canon_format.h"
#include "canon/canon_sbe.h"
#include "canon/canon_writer.h"
#include "md_gateway/md/md_types.h"
#include "refdata/mapping/instrument_mapping_loader.h"

#include <messages/ExchangeId.h>
#include <messages/ExchangeRegistry.h>
#include <messages/TradeSide.h>

#include <CLI/CLI.hpp>
#include <bpt_common/logging.h>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fast_float/fast_float.h>
#include <fmt/format.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {

/// CSV row from the OKX trades archive. All fields owned by string_view
/// pointing into the line buffer — caller must consume before re-reading
/// the buffer.
struct OkxTradeRow {
    std::string_view instrument_name;
    std::string_view trade_id;  // not used downstream, kept for diagnostics
    std::string_view side;
    std::string_view price;
    std::string_view size;
    std::string_view created_time_ms;
};

/// Parse a single CSV line into 6 fields. Returns false on a malformed
/// row (wrong column count). Does not unquote — OKX never quotes.
bool parse_csv_row(std::string_view line, OkxTradeRow& out) {
    std::string_view fields[6];
    std::size_t pos = 0;
    int idx = 0;
    while (idx < 6) {
        const std::size_t comma = line.find(',', pos);
        if (comma == std::string_view::npos) {
            if (idx == 5) {
                fields[idx++] = line.substr(pos);
                break;
            }
            return false;
        }
        fields[idx++] = line.substr(pos, comma - pos);
        pos = comma + 1;
    }
    if (idx != 6)
        return false;
    out.instrument_name = fields[0];
    out.trade_id = fields[1];
    out.side = fields[2];
    out.price = fields[3];
    out.size = fields[4];
    out.created_time_ms = fields[5];
    return true;
}

uint64_t parse_u64(std::string_view sv) {
    uint64_t v = 0;
    std::from_chars(sv.data(), sv.data() + sv.size(), v);
    return v;
}

double parse_double_sv(std::string_view sv) {
    // fast_float for CSV strings — same library bpt-common's hot-path MD
    // parser uses internally. Direct fast_float::from_chars here because
    // bpt-common's wrapper only takes simdjson values.
    double v = 0.0;
    fast_float::from_chars(sv.data(), sv.data() + sv.size(), v);
    return v;
}

}  // namespace

int main(int argc, char* argv[]) {
    CLI::App cli{"bpt-canon-ingest-okx-trades — OKX trades CSV → canon converter"};

    std::string csv_path;
    std::string instrument_mapping_path;
    std::string output_path;
    std::string producer_sha;

    cli.add_option("--trades", csv_path, "Path to OKX trades CSV (extracted from the daily zip)")
        ->required()
        ->check(CLI::ExistingFile);
    cli.add_option("--instrument-mapping", instrument_mapping_path, "Path to instrument_mapping.okx.json")
        ->required()
        ->check(CLI::ExistingFile);
    cli.add_option("--output", output_path, "Output .canon path")->required();
    cli.add_option("--producer-sha", producer_sha, "Git SHA stamped into the canon file header");

    CLI11_PARSE(cli, argc, argv);

    bpt::common::logging::init("bpt-canon-ingest-okx-trades");

    // Load OKX mapping, build venue_symbol → canonical_id index.
    bpt::refdata::mapping::InstrumentMappingLoader mapping;
    try {
        mapping.load(instrument_mapping_path);
    } catch (const std::exception& e) {
        std::cerr << "fatal: failed to load instrument mapping: " << e.what() << "\n";
        return 1;
    }

    const auto okx_enum = bpt::messages::ExchangeRegistry::from_name("OKX");
    if (!okx_enum) {
        std::cerr << "fatal: OKX not in ExchangeRegistry\n";
        return 1;
    }
    const uint8_t venue_id = static_cast<uint8_t>(*okx_enum);

    std::unordered_map<std::string, uint64_t> sym_to_id;
    for (const auto& e : mapping.instruments_for_venue(venue_id))
        sym_to_id.emplace(e.venue_symbol, e.canonical_id);
    bpt::common::log::info("[ingest-okx-trades] loaded {} OKX instruments from mapping", sym_to_id.size());

    // Probe the first non-header line for the date stamp on the canon
    // header. OKX uses the file's day; using row 1's ts puts us in the
    // right calendar day regardless of UTC vs local interpretation.
    std::ifstream in(csv_path);
    if (!in) {
        std::cerr << "fatal: cannot open " << csv_path << "\n";
        return 1;
    }
    std::string header_line;
    if (!std::getline(in, header_line)) {
        std::cerr << "fatal: empty input\n";
        return 1;
    }
    // Header sanity-check — fail loud if the format drifts (e.g. OKX
    // renames a column). One bad assumption silently passes far better
    // than the wrong PnL out the other side.
    const std::string_view expected_header = "instrument_name,trade_id,side,price,size,created_time";
    // OKX may include trailing \r from Windows-style line endings.
    std::string_view header_sv = header_line;
    while (!header_sv.empty() && (header_sv.back() == '\r' || header_sv.back() == '\n'))
        header_sv.remove_suffix(1);
    if (header_sv != expected_header) {
        std::cerr << "fatal: unexpected CSV header. Got: '" << header_sv << "', expected: '" << expected_header
                  << "'\n";
        return 1;
    }

    uint32_t header_date_utc = 0;

    bpt::canon::CanonWriter::Config wcfg;
    wcfg.path = output_path;
    wcfg.producer_kind = "okx-trades-csv";
    wcfg.producer_sha = producer_sha;
    wcfg.venue_id = venue_id;
    // date_utc populated after we see the first row.

    std::string line;
    OkxTradeRow row;
    uint64_t row_count = 0;
    uint64_t emitted = 0;
    uint64_t unknown_symbol = 0;
    uint64_t bad_row = 0;
    std::unordered_map<std::string, uint64_t> unknown_symbol_counts;

    // First row peek for date.
    if (std::getline(in, line)) {
        std::string_view sv = line;
        while (!sv.empty() && (sv.back() == '\r' || sv.back() == '\n'))
            sv.remove_suffix(1);
        if (parse_csv_row(sv, row)) {
            const uint64_t ts_ms = parse_u64(row.created_time_ms);
            const std::time_t t = static_cast<std::time_t>(ts_ms / 1000);
            std::tm tm{};
            gmtime_r(&t, &tm);
            header_date_utc = bpt::canon::pack_date(static_cast<uint16_t>(tm.tm_year + 1900),
                                                    static_cast<uint8_t>(tm.tm_mon + 1),
                                                    static_cast<uint8_t>(tm.tm_mday));
        }
    }
    wcfg.date_utc = header_date_utc;

    bpt::canon::CanonWriter writer(wcfg);
    if (!writer.open()) {
        std::cerr << "fatal: failed to open canon writer at " << output_path << "\n";
        return 1;
    }

    // Re-open input so we process from row 1 onward.
    in.clear();
    in.seekg(0);
    std::getline(in, header_line);  // skip header again

    while (std::getline(in, line)) {
        ++row_count;
        std::string_view sv = line;
        while (!sv.empty() && (sv.back() == '\r' || sv.back() == '\n'))
            sv.remove_suffix(1);
        if (!parse_csv_row(sv, row)) {
            ++bad_row;
            continue;
        }
        const std::string symbol_key{row.instrument_name};
        auto it = sym_to_id.find(symbol_key);
        if (it == sym_to_id.end()) {
            ++unknown_symbol;
            ++unknown_symbol_counts[symbol_key];
            continue;
        }

        bpt::md_gateway::md::MdTrade trade{};
        trade.timestamp_ns = parse_u64(row.created_time_ms) * 1'000'000ULL;
        trade.instrument_id = it->second;
        trade.price = parse_double_sv(row.price);
        trade.qty = parse_double_sv(row.size);
        trade.side = (row.side == "buy") ? bpt::messages::TradeSide::BUY : bpt::messages::TradeSide::SELL;

        char buf[bpt::canon::CanonScratch::kTradeSize];
        const std::size_t n = bpt::canon::encode_trade(trade, /*seq_num=*/++emitted, buf, sizeof(buf));
        if (n == 0)
            continue;
        writer.write_event(trade.timestamp_ns, bpt::canon::EventType::TRADE, std::string_view{buf, n});
    }

    writer.flush();
    writer.close();

    fmt::print("[ingest-okx-trades] read {} CSV rows, emitted {} canon TRADE events\n", row_count, emitted);
    if (bad_row > 0)
        fmt::print("[ingest-okx-trades] {} malformed rows skipped\n", bad_row);
    if (unknown_symbol > 0) {
        fmt::print("[ingest-okx-trades] {} rows skipped (instrument not in OKX mapping):\n", unknown_symbol);
        for (const auto& [s, c] : unknown_symbol_counts)
            fmt::print("    {} x{}\n", s, c);
    }
    fmt::print("[ingest-okx-trades] wrote {} bytes to {}\n", writer.bytes_written(), writer.path());

    return 0;
}
