/// bpt-canon-merge — k-way merge of multiple `.canon` files into one
/// timestamp-ordered output.
///
/// The deterministic backtester reads a single canon file and assumes
/// records are monotonic in ts. When multiple producers feed the same
/// venue/day (e.g. `bpt-canon-ingest-okx-trades` + `-okx-l2` write
/// independent files), this tool merges them.
///
/// Algorithm: open one CanonReader per input, prime each with its
/// first record, then in a loop pick the reader whose next record has
/// the smallest ts, write it, advance. Ties broken by input order
/// (stable). Memory cost is one record buffer per input, so it
/// streams cleanly even when inputs are multi-GB.

#include "canon/canon_format.h"
#include "canon/canon_reader.h"
#include "canon/canon_writer.h"

#include <CLI/CLI.hpp>
#include <bpt_common/logging.h>
#include <cstdint>
#include <cstring>
#include <fmt/format.h>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

int main(int argc, char* argv[]) {
    CLI::App cli{"bpt-canon-merge — k-way ts-merge of canon files"};

    std::vector<std::string> input_paths;
    std::string output_path;
    std::string producer_sha;
    std::string producer_kind = "merge";

    cli.add_option("--input", input_paths, "Input .canon files (one or more, will be merged by ts)")
        ->required()
        ->check(CLI::ExistingFile);
    cli.add_option("--output", output_path, "Output merged .canon path")->required();
    cli.add_option("--producer-kind",
                   producer_kind,
                   "producer_kind to stamp on the merged file's header (default: 'merge')")
        ->capture_default_str();
    cli.add_option("--producer-sha", producer_sha, "Git SHA stamped on the merged file's header");

    CLI11_PARSE(cli, argc, argv);

    bpt::common::logging::init("bpt-canon-merge");

    // Open all readers, validate headers, capture metadata for output.
    std::vector<std::unique_ptr<bpt::canon::CanonReader>> readers;
    std::vector<std::optional<bpt::canon::CanonRecord>> heads;
    bpt::canon::CanonFileHeader first_header{};
    bool first_header_set = false;
    for (const auto& p : input_paths) {
        auto r = std::make_unique<bpt::canon::CanonReader>(p);
        if (!r->ok()) {
            std::cerr << "fatal: cannot open " << p << " as canon file\n";
            return 1;
        }
        if (!first_header_set) {
            first_header = r->header();
            first_header_set = true;
        } else {
            // Refuse to merge across venues — producers should keep one
            // venue per file. Date and producer_kind can differ (that's
            // the whole point).
            if (r->header().venue_id != first_header.venue_id) {
                std::cerr << "fatal: cannot merge across venues. " << p
                          << " has venue_id=" << static_cast<int>(r->header().venue_id)
                          << " but first input has venue_id=" << static_cast<int>(first_header.venue_id) << "\n";
                return 1;
            }
        }
        heads.push_back(r->next());
        readers.push_back(std::move(r));
    }

    bpt::canon::CanonWriter::Config wcfg;
    wcfg.path = output_path;
    wcfg.producer_kind = producer_kind;
    wcfg.producer_sha = producer_sha;
    wcfg.venue_id = first_header.venue_id;
    wcfg.date_utc = first_header.date_utc;
    wcfg.buffer_bytes = 4u << 20;

    bpt::canon::CanonWriter writer(wcfg);
    if (!writer.open()) {
        std::cerr << "fatal: failed to open " << output_path << "\n";
        return 1;
    }

    uint64_t emitted = 0;
    uint64_t last_emit_ts = 0;
    bool warned_non_monotonic = false;

    while (true) {
        // Find the reader with the smallest head ts.
        std::size_t pick = readers.size();
        uint64_t best_ts = 0;
        for (std::size_t i = 0; i < readers.size(); ++i) {
            if (!heads[i].has_value())
                continue;
            const uint64_t ts = heads[i]->ts_ns;
            if (pick == readers.size() || ts < best_ts) {
                pick = i;
                best_ts = ts;
            }
        }
        if (pick == readers.size())
            break;  // all readers drained

        const auto& rec = *heads[pick];
        if (rec.ts_ns < last_emit_ts && !warned_non_monotonic) {
            // Inputs are supposed to be ts-monotonic internally; if not,
            // the merged output won't be either. Surface once so the
            // operator knows.
            bpt::common::log::warn(
                "[merge] non-monotonic input detected (rec ts={} < last emit ts={}). Outputs may not be ts-sorted.",
                rec.ts_ns,
                last_emit_ts);
            warned_non_monotonic = true;
        }
        const std::string_view sbe(reinterpret_cast<const char*>(rec.sbe.data()), rec.sbe.size());
        writer.write_event(rec.ts_ns, rec.type, sbe);
        last_emit_ts = rec.ts_ns;
        ++emitted;

        heads[pick] = readers[pick]->next();
    }

    writer.flush();
    writer.close();

    fmt::print("[merge] merged {} input files → {} events → {} bytes at {}\n",
               input_paths.size(),
               emitted,
               writer.bytes_written(),
               writer.path());
    return 0;
}
