/// bpt-canon-replay — decodes raw `.wslog` venue captures and writes
/// the resulting normalised events to a `.canon` file.
///
/// One process, one venue, one output file. The decoder used is the same
/// one bpt-md-gateway runs live, so the bytes that land in `.canon` are
/// bit-identical to what the live system would have published to Aeron
/// for the same input. That equivalence is the whole point — it lets the
/// deterministic backtester read either format and get the same
/// behaviour, and it gives the schema layer (canon) a path to absorb
/// third-party historical archives that never had a `.wslog` form
/// (OKX, Tardis, etc. — to be added as sibling producers).
///
/// Today: HYPERLIQUID only. Adding OKX/Binance/Deribit is a matter of
/// templating the decoder selection on the --venue flag; the rest of
/// this binary is venue-agnostic.

#include "bpt_common/recorder/wslog_format.h"
#include "canon/canon_format.h"
#include "canon/canon_recording_publisher.h"
#include "canon/canon_writer.h"
#include "md_gateway/adapter/common/subscription_map.h"
#include "md_gateway/adapter/hyperliquid/hyperliquid_md_decoder.h"
#include "md_gateway/messaging/publishers/api/funding_rate_publisher.h"
#include "md_gateway/messaging/publishers/api/instrument_stats_publisher.h"
#include "refdata/mapping/instrument_mapping_loader.h"

#include <messages/ExchangeId.h>
#include <messages/ExchangeRegistry.h>

#include <CLI/CLI.hpp>
#include <bpt_common/logging.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fmt/format.h>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct WslogRecord {
    uint64_t ts_ns;
    bpt::common::recorder::RecordType type;
    std::vector<uint8_t> payload;
};

/// Standalone wslog reader. The backtester's own reader sits inside
/// bpt-backtester (`backtester/harness/wslog_reader.h`) and we don't
/// want to depend on the backtester from a producer binary.
class WslogReader {
public:
    explicit WslogReader(const std::string& path) { fp_ = std::fopen(path.c_str(), "rb"); }
    ~WslogReader() {
        if (fp_)
            std::fclose(fp_);
    }
    WslogReader(const WslogReader&) = delete;
    WslogReader& operator=(const WslogReader&) = delete;

    [[nodiscard]] bool ok() const noexcept { return fp_ != nullptr; }

    std::optional<WslogRecord> next() {
        if (!fp_)
            return std::nullopt;
        WslogRecord rec{};
        uint8_t type_byte;
        uint32_t length;
        if (std::fread(&rec.ts_ns, sizeof(rec.ts_ns), 1, fp_) != 1)
            return std::nullopt;
        if (std::fread(&type_byte, sizeof(type_byte), 1, fp_) != 1)
            return std::nullopt;
        if (std::fread(&length, sizeof(length), 1, fp_) != 1)
            return std::nullopt;
        rec.type = static_cast<bpt::common::recorder::RecordType>(type_byte);
        rec.payload.resize(length);
        if (length > 0 && std::fread(rec.payload.data(), 1, length, fp_) != length)
            return std::nullopt;
        return rec;
    }

private:
    std::FILE* fp_{nullptr};
};

uint32_t pack_date_from_ns(uint64_t ns) {
    const std::time_t t = static_cast<std::time_t>(ns / 1'000'000'000ULL);
    std::tm tm{};
    gmtime_r(&t, &tm);
    return bpt::canon::pack_date(static_cast<uint16_t>(tm.tm_year + 1900),
                                 static_cast<uint8_t>(tm.tm_mon + 1),
                                 static_cast<uint8_t>(tm.tm_mday));
}

}  // namespace

int main(int argc, char* argv[]) {
    CLI::App cli{"bpt-canon-replay — wslog → canon converter"};

    std::vector<std::string> wslog_paths;
    std::string instrument_mapping;
    std::string output_path;
    std::string venue_name = "HYPERLIQUID";
    std::string producer_sha;

    cli.add_option("--wslog", wslog_paths, "One or more .wslog files to convert (in timestamp order)")
        ->required()
        ->check(CLI::ExistingFile);
    cli.add_option("--instrument-mapping", instrument_mapping, "Path to instrument_mapping.<venue>.json")
        ->required()
        ->check(CLI::ExistingFile);
    cli.add_option("--output", output_path, "Output .canon path")->required();
    cli.add_option("--venue", venue_name, "Venue tag (only HYPERLIQUID supported today)")->capture_default_str();
    cli.add_option("--producer-sha",
                   producer_sha,
                   "Git SHA stamped into the canon file header (truncated to 40 chars)");

    CLI11_PARSE(cli, argc, argv);

    bpt::common::logging::init("bpt-canon-replay");

    if (venue_name != "HYPERLIQUID") {
        std::cerr << "fatal: only --venue HYPERLIQUID is supported in this binary today\n";
        return 1;
    }

    const auto exch = bpt::messages::ExchangeRegistry::from_name(venue_name);
    if (!exch) {
        std::cerr << "fatal: unknown venue '" << venue_name << "'\n";
        return 1;
    }
    const uint8_t venue_id = static_cast<uint8_t>(*exch);

    // Load instrument mapping and populate the HL SubscriptionMap so the
    // venue decoder can resolve `coin` strings → canonical instrument_id.
    // Subscribe to every instrument the mapping carries for this venue;
    // unlike a strategy, the replay producer wants every parsed frame.
    bpt::refdata::mapping::InstrumentMappingLoader mapping;
    try {
        mapping.load(instrument_mapping);
    } catch (const std::exception& e) {
        std::cerr << "fatal: failed to load instrument mapping: " << e.what() << "\n";
        return 1;
    }

    // Subscribe with full depth so the HL decoder publishes BOTH MdBbo
    // *and* MdOrderBook for each `l2Book` frame. The decoder's branch at
    // hyperliquid_md_decoder.h:146 only emits the L2 book when
    // `depth_cap > 0`; without it, canon would record only top-of-book
    // and downstream consumers (e.g. the backtester's matching engine,
    // which needs depth>1 to seed venue slots at lower price levels for
    // queue-aware fills) would silently lose data.
    //
    // Replay is a producer, not a strategy — record the maximum the
    // decoder is willing to emit, then let consumers filter on read.
    bpt::md_gateway::adapter::SubscriptionMap subs;
    std::size_t subscribed = 0;
    for (const auto& e : mapping.instruments_for_venue(venue_id)) {
        subs.subscribe(e.canonical_id, e.venue_symbol, /*depth=*/bpt::md_gateway::md::kMaxBookLevels);
        ++subscribed;
    }
    bpt::common::log::info("[replay] subscribed {} {} instruments for canon replay", subscribed, venue_name);

    if (subscribed == 0) {
        std::cerr << "fatal: instrument mapping has no entries for venue " << venue_name << "\n";
        return 1;
    }

    // Date stamp on the canon header — derived from the first wslog
    // record's timestamp so the header reflects the data, not the
    // replay wallclock.
    uint32_t header_date_utc = 0;
    {
        WslogReader probe(wslog_paths.front());
        if (!probe.ok()) {
            std::cerr << "fatal: cannot open " << wslog_paths.front() << "\n";
            return 1;
        }
        if (auto first = probe.next())
            header_date_utc = pack_date_from_ns(first->ts_ns);
    }

    bpt::canon::CanonWriter::Config wcfg;
    wcfg.path = output_path;
    wcfg.producer_kind = "wslog-replay";
    wcfg.producer_sha = producer_sha;
    wcfg.venue_id = venue_id;
    wcfg.date_utc = header_date_utc;

    bpt::canon::CanonWriter writer(wcfg);
    if (!writer.open()) {
        std::cerr << "fatal: failed to open canon writer at " << output_path << "\n";
        return 1;
    }

    bpt::canon::CanonRecordingPublisher recorder(writer);

    // Funding-rate path: HL emits funding via `activeAssetCtx` frames. The
    // decoder dispatches funding via a callback (not the publish() chain
    // because funding rides a separate Aeron stream live). Route it into
    // our recorder's funding overload too.
    bpt::md_gateway::messaging::FundingRateCallback funding_cb =
        [&recorder, venue_id_local = *exch](const bpt::md_gateway::messaging::FundingRateUpdate& fr) {
            // Decoder doesn't stamp exchange_id (it's a venue-scoped thing);
            // patch it here so the canon file reflects the right venue.
            bpt::md_gateway::messaging::FundingRateUpdate stamped = fr;
            stamped.exchange_id = venue_id_local;
            recorder.publish(stamped);
        };
    bpt::md_gateway::messaging::InstrumentStatsCallback stats_cb =
        [](const bpt::md_gateway::messaging::InstrumentStatsUpdate&) {
        };

    // Decoder is templated on Pub — here Pub = CanonRecordingPublisher.
    bpt::md_gateway::adapter::HyperliquidMdDecoder<bpt::canon::CanonRecordingPublisher> decoder(subs);

    uint64_t frame_count = 0;
    uint64_t last_ts_ns = 0;
    for (const auto& path : wslog_paths) {
        WslogReader r(path);
        if (!r.ok()) {
            bpt::common::log::warn("[replay] failed to open {}; skipping", path);
            continue;
        }
        while (auto rec = r.next()) {
            last_ts_ns = rec->ts_ns;
            if (rec->type != bpt::common::recorder::RecordType::WS_FRAME)
                continue;
            std::string_view payload(reinterpret_cast<const char*>(rec->payload.data()), rec->payload.size());
            decoder.decode(payload, rec->ts_ns, recorder, funding_cb, stats_cb);
            ++frame_count;
        }
    }

    writer.flush();
    writer.close();

    fmt::print("[replay] read {} WS_FRAME records from {} file(s)\n", frame_count, wslog_paths.size());
    fmt::print("[replay] emitted: bbo={} trade={} book={} funding={}\n",
               recorder.bbo_count(),
               recorder.trade_count(),
               recorder.book_count(),
               recorder.funding_count());
    fmt::print("[replay] wrote {} bytes to {}\n", writer.bytes_written(), writer.path());
    (void)last_ts_ns;

    return 0;
}
