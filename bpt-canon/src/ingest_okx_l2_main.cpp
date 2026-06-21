/// bpt-canon-ingest-okx-l2 — converts OKX's free historical L2 orderbook
/// NDJSON archive (one file per instrument-family per day) into canon
/// BBO + BOOK events.
///
/// Input file is the OKX `books` channel dumped raw to NDJSON. Each line:
///
///   {"instId":"BTC-USDT-SWAP","action":"snapshot",
///    "ts":"1777507200002",
///    "asks":[["price","size","numOrders"], ...],   // ascending
///    "bids":[["price","size","numOrders"], ...]}   // descending
///
/// Two action types:
///   - `snapshot` — full book replacement (typically every ~15 min)
///   - `update`   — list of changed levels only. size="0" means remove.
///
/// Strategy: maintain the full book in memory (price → size map, both
/// sides). Apply each line. After each apply, emit:
///   1. Canon BBO from top-of-book (best bid + best ask)
///   2. Canon BOOK with top-N levels (N capped at MdOrderBook::kMaxLevels)
///
/// Both events share the same canon timestamp (the line's `ts`).

#include "canon/canon_format.h"
#include "canon/canon_sbe.h"
#include "canon/canon_writer.h"
#include "md_gateway/md/md_types.h"
#include "refdata/mapping/instrument_mapping_loader.h"

#include <messages/ExchangeId.h>
#include <messages/ExchangeRegistry.h>

#include <CLI/CLI.hpp>
#include <bpt_common/logging.h>
#include <bpt_common/util/parse_double.h>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fast_float/fast_float.h>
#include <fmt/format.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <simdjson.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {

/// In-memory book state — full depth, applied incrementally.
///
/// asks ordered ascending (best ask = first); bids ordered descending
/// (best bid = first) via std::greater. Using std::map (red-black tree)
/// for O(log n) inserts/removes — at 400 levels per side that's tiny
/// and the deterministic iteration order matters more than raw speed.
struct BookState {
    std::map<double, double> asks;
    std::map<double, double, std::greater<>> bids;

    void apply_update_array(simdjson::ondemand::array arr, bool is_bids) {
        for (auto lvl_res : arr) {
            simdjson::ondemand::array lvl_arr;
            if (lvl_res.get_array().get(lvl_arr))
                continue;
            apply_level(lvl_arr, is_bids);
        }
    }

    void apply_level(simdjson::ondemand::array lvl, bool is_bids) {
        double price = 0.0;
        double size = 0.0;
        int idx = 0;
        for (auto field_res : lvl) {
            std::string_view sv;
            if (field_res.get_string().get(sv))
                return;
            if (idx == 0)
                fast_float::from_chars(sv.data(), sv.data() + sv.size(), price);
            else if (idx == 1)
                fast_float::from_chars(sv.data(), sv.data() + sv.size(), size);
            // idx == 2: numOrders, ignored
            ++idx;
            if (idx >= 3)
                break;
        }
        if (price <= 0.0)
            return;

        if (is_bids) {
            if (size <= 0.0)
                bids.erase(price);
            else
                bids[price] = size;
        } else {
            if (size <= 0.0)
                asks.erase(price);
            else
                asks[price] = size;
        }
    }
};

}  // namespace

int main(int argc, char* argv[]) {
    CLI::App cli{"bpt-canon-ingest-okx-l2 — OKX L2 orderbook NDJSON → canon converter"};

    std::string ndjson_path;
    std::string instrument_mapping_path;
    std::string output_path;
    std::string producer_sha;
    std::size_t depth_limit = bpt::md_gateway::md::kMaxBookLevels;
    bool emit_bbo = true;
    bool emit_book = true;

    cli.add_option("--orderbook", ndjson_path, "Path to OKX L2 NDJSON (untarred .data file)")
        ->required()
        ->check(CLI::ExistingFile);
    cli.add_option("--instrument-mapping", instrument_mapping_path, "Path to instrument_mapping.okx.json")
        ->required()
        ->check(CLI::ExistingFile);
    cli.add_option("--output", output_path, "Output .canon path")->required();
    cli.add_option("--producer-sha", producer_sha, "Git SHA stamped into the canon file header");
    cli.add_option("--depth",
                   depth_limit,
                   "Max L2 levels per side written to canon (default: kMaxBookLevels = 20). The OKX archive has "
                   "400; we down-sample to what the live decoder emits so backtester behaviour stays comparable.")
        ->capture_default_str();
    cli.add_flag("!--no-bbo", emit_bbo, "Skip emitting canon BBO events");
    cli.add_flag("!--no-book", emit_book, "Skip emitting canon BOOK events");

    CLI11_PARSE(cli, argc, argv);

    bpt::common::logging::init("bpt-canon-ingest-okx-l2");

    if (depth_limit > bpt::md_gateway::md::kMaxBookLevels) {
        std::cerr << "fatal: --depth " << depth_limit << " > kMaxBookLevels " << bpt::md_gateway::md::kMaxBookLevels
                  << "\n";
        return 1;
    }

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

    // Probe first line for date stamp on the canon header.
    uint32_t header_date_utc = 0;
    {
        std::ifstream probe(ndjson_path);
        std::string first_line;
        if (std::getline(probe, first_line)) {
            simdjson::ondemand::parser parser;
            simdjson::padded_string padded(first_line);
            simdjson::ondemand::document doc;
            if (parser.iterate(padded).get(doc) == simdjson::SUCCESS) {
                std::string_view ts_str;
                if (doc["ts"].get_string().get(ts_str) == simdjson::SUCCESS) {
                    uint64_t ts_ms = 0;
                    std::from_chars(ts_str.data(), ts_str.data() + ts_str.size(), ts_ms);
                    const std::time_t t = static_cast<std::time_t>(ts_ms / 1000);
                    std::tm tm{};
                    gmtime_r(&t, &tm);
                    header_date_utc = bpt::canon::pack_date(static_cast<uint16_t>(tm.tm_year + 1900),
                                                            static_cast<uint8_t>(tm.tm_mon + 1),
                                                            static_cast<uint8_t>(tm.tm_mday));
                }
            }
        }
    }

    bpt::canon::CanonWriter::Config wcfg;
    wcfg.path = output_path;
    wcfg.producer_kind = "okx-l2-ndjson";
    wcfg.producer_sha = producer_sha;
    wcfg.venue_id = venue_id;
    wcfg.date_utc = header_date_utc;
    wcfg.buffer_bytes = 4u << 20;  // 4 MiB userspace buffer — bigger blobs

    bpt::canon::CanonWriter writer(wcfg);
    if (!writer.open()) {
        std::cerr << "fatal: failed to open canon writer at " << output_path << "\n";
        return 1;
    }

    BookState book;
    std::ifstream in(ndjson_path);
    simdjson::ondemand::parser parser;

    // The full L2 NDJSON is multi-GB for a busy instrument-day. Stream
    // it line-by-line — never load the whole thing.
    std::string line;
    line.reserve(1 << 20);  // 1 MiB — snapshot lines can be > 64 KiB

    uint64_t line_count = 0;
    uint64_t snapshots = 0;
    uint64_t updates = 0;
    uint64_t skipped_unknown_inst = 0;
    uint64_t bbo_emitted = 0;
    uint64_t book_emitted = 0;
    uint64_t bad_line = 0;

    while (std::getline(in, line)) {
        ++line_count;

        // simdjson needs the buffer to have at least SIMDJSON_PADDING
        // (64 bytes) of trailing slack. Pad the line ourselves to avoid
        // copying into a padded_string on the hot path.
        std::size_t orig_size = line.size();
        // Trim trailing \r from windows line endings.
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
            --orig_size;
        }
        line.resize(orig_size + simdjson::SIMDJSON_PADDING, '\0');

        simdjson::ondemand::document doc;
        if (parser.iterate(line.data(), orig_size, line.size()).get(doc) != simdjson::SUCCESS) {
            ++bad_line;
            line.resize(orig_size);
            continue;
        }

        std::string_view inst_id_sv;
        if (doc["instId"].get_string().get(inst_id_sv) != simdjson::SUCCESS) {
            ++bad_line;
            line.resize(orig_size);
            continue;
        }
        const std::string inst_key{inst_id_sv};
        auto it = sym_to_id.find(inst_key);
        if (it == sym_to_id.end()) {
            ++skipped_unknown_inst;
            line.resize(orig_size);
            continue;
        }
        const uint64_t instrument_id = it->second;

        std::string_view action_sv;
        if (doc["action"].get_string().get(action_sv) != simdjson::SUCCESS) {
            ++bad_line;
            line.resize(orig_size);
            continue;
        }

        std::string_view ts_sv;
        if (doc["ts"].get_string().get(ts_sv) != simdjson::SUCCESS) {
            ++bad_line;
            line.resize(orig_size);
            continue;
        }
        uint64_t ts_ms = 0;
        std::from_chars(ts_sv.data(), ts_sv.data() + ts_sv.size(), ts_ms);
        const uint64_t ts_ns = ts_ms * 1'000'000ULL;

        // simdjson ondemand is forward-only: each array must be iterated
        // before the next sibling field is accessed, otherwise the first
        // array's iterator is silently invalidated. Process asks first,
        // then bids — matching the field order in the OKX JSON.
        const bool is_snapshot = (action_sv == "snapshot");
        if (is_snapshot) {
            ++snapshots;
            book.asks.clear();
            book.bids.clear();
        } else {
            ++updates;
        }

        simdjson::ondemand::array asks_arr;
        if (doc["asks"].get_array().get(asks_arr) == simdjson::SUCCESS)
            book.apply_update_array(asks_arr, /*is_bids=*/false);
        simdjson::ondemand::array bids_arr;
        if (doc["bids"].get_array().get(bids_arr) == simdjson::SUCCESS)
            book.apply_update_array(bids_arr, /*is_bids=*/true);

        line.resize(orig_size);

        if (book.asks.empty() && book.bids.empty())
            continue;

        // ── Emit BBO ──
        if (emit_bbo) {
            bpt::md_gateway::md::MdBbo bbo{};
            bbo.timestamp_ns = ts_ns;
            bbo.instrument_id = instrument_id;
            if (!book.bids.empty()) {
                const auto& [p, s] = *book.bids.begin();  // greater<> → best bid first
                bbo.bid_price = p;
                bbo.bid_qty = s;
            }
            if (!book.asks.empty()) {
                const auto& [p, s] = *book.asks.begin();  // less<> → best ask first
                bbo.ask_price = p;
                bbo.ask_qty = s;
            }
            char buf[bpt::canon::CanonScratch::kBboSize];
            const std::size_t n = bpt::canon::encode_bbo(bbo, ++bbo_emitted, buf, sizeof(buf));
            if (n > 0)
                writer.write_event(ts_ns, bpt::canon::EventType::BBO, std::string_view{buf, n});
        }

        // ── Emit L2 BOOK ──
        if (emit_book) {
            bpt::md_gateway::md::MdOrderBook ob{};
            ob.timestamp_ns = ts_ns;
            ob.instrument_id = instrument_id;
            for (auto bit = book.bids.begin(); bit != book.bids.end() && ob.bids.size() < depth_limit; ++bit)
                ob.bids.emplace_back(bit->first, bit->second);
            for (auto ait = book.asks.begin(); ait != book.asks.end() && ob.asks.size() < depth_limit; ++ait)
                ob.asks.emplace_back(ait->first, ait->second);
            char buf[bpt::canon::CanonScratch::kBookSize];
            const std::size_t n = bpt::canon::encode_book(ob, ++book_emitted, buf, sizeof(buf));
            if (n > 0)
                writer.write_event(ts_ns, bpt::canon::EventType::BOOK, std::string_view{buf, n});
        }
    }

    writer.flush();
    writer.close();

    fmt::print("[ingest-okx-l2] read {} NDJSON lines: {} snapshots + {} updates\n", line_count, snapshots, updates);
    if (bad_line > 0)
        fmt::print("[ingest-okx-l2] {} malformed lines skipped\n", bad_line);
    if (skipped_unknown_inst > 0)
        fmt::print("[ingest-okx-l2] {} lines skipped (instrument not in OKX mapping)\n", skipped_unknown_inst);
    fmt::print("[ingest-okx-l2] emitted: bbo={} book={}\n", bbo_emitted, book_emitted);
    fmt::print("[ingest-okx-l2] wrote {} bytes to {}\n", writer.bytes_written(), writer.path());

    return 0;
}
