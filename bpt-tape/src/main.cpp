/// bpt-tape — "the tape": captures venue WS frames to disk by importing
/// bpt-md-gateway's adapter library and substituting a recording subclass
/// that tees raw bytes via bpt::common::recorder::RawSpool.
///
/// mdgw and refdata service binaries are unchanged; this process owns the
/// recording feature in full. No Aeron data publication; a no-op publisher
/// satisfies the adapter's Pub template parameter so parsing still runs
/// (cheap on the recording host) and the wire pipeline behaves identically
/// to live trading at every layer except the disk tap.
///
/// Refdata REST snapshot capture (instrument list, fee schedule, listing
/// events) is a planned extension along the same RawSpool path — RawSpool
/// is already designed to accept REST response bodies as well as WS frames.

#include "tape/adapter/recording_mdgw_adapters.h"
#include "tape/config/settings.h"
#include "tape/refdata/refdata_poller.h"
#include "bpt_common/recorder/raw_spool.h"
#include "md_gateway/md/md_types.h"
#include "md_gateway/adapter/common/i_adapter.h"
#include "refdata/mapping/instrument_mapping_loader.h"
#include <messages/ExchangeRegistry.h>

#include <CLI/CLI.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fmt/format.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <bpt_app/app.h>
#include <bpt_common/logging.h>
#include <bpt_common/signal.h>

namespace bpt::tape {

namespace {

std::string lowercase_venue(const std::string& exchange) {
    std::string out = exchange;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}

uint64_t wall_now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

// Drops every published message. Recording host has no downstream consumers
// of the parsed SBE — the disk tap on raw WS frames is the only output.
// Concrete (non-virtual) — venue adapters are templated on Pub so the
// publish() chain compiles down to dead branches the optimizer drops.
class NoopMdPublisher {
public:
    void publish(const bpt::md_gateway::md::MdBbo&) {}
    void publish(const bpt::md_gateway::md::MdTrade&) {}
    void publish(const bpt::md_gateway::md::MdOrderBook&) {}
    uint64_t drop_count() const { return 0; }
};

class RecorderService : public bpt::app::IService {
public:
    RecorderService(config::Settings settings,
                    const bpt::common::util::Topology& topology)
        : settings_(std::move(settings)), topology_(topology) {
        auto pub = std::make_shared<NoopMdPublisher>();

        for (const auto& a_cfg : settings_.mdgw_adapters) {
            const std::string venue_tag = lowercase_venue(a_cfg.exchange);
            auto spool = std::make_shared<bpt::common::recorder::RawSpool>(
                bpt::common::recorder::RawSpool::Config{
                    .root_dir = settings_.recording.output_dir,
                    .venue_tag = venue_tag,
                    .rotate_interval_seconds = settings_.recording.rotate_interval_seconds,
                    .buffer_bytes = settings_.recording.buffer_bytes,
                    .flush_interval_ns =
                        static_cast<uint64_t>(settings_.recording.fsync_interval_ms) *
                        1'000'000ULL,
                });
            // SESSION_START with config snapshot — pid + venue + WS endpoint.
            const std::string snapshot = fmt::format(
                R"({{"pid":{},"exchange":"{}","ws":"{}://{}:{}{}"}})",
                ::getpid(), a_cfg.exchange,
                a_cfg.use_tls ? "wss" : "ws",
                a_cfg.ws_host, a_cfg.ws_port, a_cfg.ws_path);
            spool->write_marker(wall_now_ns(),
                                bpt::common::recorder::RecordType::SESSION_START,
                                snapshot);
            spool->flush();

            const auto exch_id = bpt::messages::ExchangeRegistry::from_name(a_cfg.exchange);
            if (!exch_id) {
                throw std::runtime_error(fmt::format(
                    "Unknown exchange '{}' in bpt-tape config — not in messages/exchanges.yaml",
                    a_cfg.exchange));
            }
            std::shared_ptr<bpt::md_gateway::adapter::IAdapter> adapter;
            switch (*exch_id) {
                case bpt::messages::ExchangeId::BINANCE:
                    adapter = std::make_shared<adapter::RecordingBinanceMdAdapter<NoopMdPublisher>>(spool, a_cfg, pub);
                    break;
                case bpt::messages::ExchangeId::OKX:
                    adapter = std::make_shared<adapter::RecordingOkxMdAdapter<NoopMdPublisher>>(spool, a_cfg, pub);
                    break;
                case bpt::messages::ExchangeId::HYPERLIQUID:
                    adapter = std::make_shared<adapter::RecordingHyperliquidMdAdapter<NoopMdPublisher>>(spool, a_cfg, pub);
                    break;
                case bpt::messages::ExchangeId::DERIBIT:
                    adapter = std::make_shared<adapter::RecordingDeribitMdAdapter<NoopMdPublisher>>(spool, a_cfg, pub);
                    break;
                default:
                    throw std::runtime_error(fmt::format(
                        "Exchange '{}' is in the registry but bpt-tape has no recording adapter for it",
                        a_cfg.exchange));
            }

            // Connection-state markers via the existing on_connect/on_disconnect
            // hooks. First connect after process start is bracketed by
            // SESSION_START rather than WS_RECONNECT — gate the marker on
            // having seen at least one prior disconnect.
            auto state = std::make_shared<ConnState>();
            adapter->on_connect = [spool, state]() {
                if (!state->was_disconnected) return;
                ++state->reconnect_count;
                spool->write_marker(wall_now_ns(),
                                    bpt::common::recorder::RecordType::WS_RECONNECT,
                                    fmt::format(R"({{"attempt":{}}})", state->reconnect_count));
                spool->flush();
                state->was_disconnected = false;
            };
            adapter->on_disconnect = [spool, state]() {
                spool->write_marker(wall_now_ns(),
                                    bpt::common::recorder::RecordType::WS_DISCONNECT,
                                    fmt::format(R"({{"attempt":{}}})",
                                                state->reconnect_count + 1));
                spool->flush();
                state->was_disconnected = true;
            };

            adapter->set_topology(topology_);
            adapter->start();
            spools_.push_back(spool);
            adapters_per_venue_[a_cfg.exchange] = adapter;
            adapters_.push_back(std::move(adapter));
            bpt::common::log::info("bpt-tape: started recording adapter for {} → {}",
                                   a_cfg.exchange, spool->current_path());
        }

        // Anything below this point that throws would leave the just-started
        // adapter threads orphaned (AdapterBase has no joining destructor —
        // by design, since stop() is the lifecycle handle the framework
        // calls). Wrap in a try so we can stop cleanly before re-throwing.
        try {

        // Build the universe by reading the canonical instrument-mapping
        // JSON (same file bpt-refdata reads on the trading host) and
        // filtering per venue. No refdata service needed on the recording
        // host — the mapping logic is imported as a library
        // (//bpt-refdata:mapping_lib).
        bpt::refdata::mapping::InstrumentMappingLoader mapping;
        mapping.load(settings_.instrument_mapping_path);
        bpt::common::log::info("bpt-tape: loaded instrument mapping from {} ({} instruments)",
                               settings_.instrument_mapping_path,
                               mapping.instrument_count());

        const auto& filter = settings_.universe_filter;
        const auto matches_filter = [&filter](const auto& entry) {
            if (!filter.inst_types.empty()) {
                if (std::find(filter.inst_types.begin(), filter.inst_types.end(),
                              entry.info.type) == filter.inst_types.end())
                    return false;
            }
            if (std::find(filter.exclude_bases.begin(), filter.exclude_bases.end(),
                          entry.info.base) != filter.exclude_bases.end())
                return false;
            return true;
        };

        size_t n_subscribed = 0;
        for (const auto& [venue_name, adapter] : adapters_per_venue_) {
            const auto venue_id = bpt::messages::ExchangeRegistry::from_name(venue_name);
            if (!venue_id) continue;  // unreachable: ctor validates above
            const auto entries = mapping.instruments_for_venue(static_cast<uint8_t>(*venue_id));
            for (const auto& e : entries) {
                if (!matches_filter(e)) continue;
                adapter->subscribe(e.canonical_id, e.venue_symbol, filter.default_depth);
                ++n_subscribed;
            }
        }
        bpt::common::log::info("bpt-tape: subscribed {} symbols across {} adapters",
                               n_subscribed, adapters_.size());
        } catch (...) {
            // Reraise after stopping so adapter threads join cleanly —
            // otherwise std::thread destructor calls std::terminate.
            for (auto& a : adapters_) a->stop();
            throw;
        }

        // Refdata REST pollers — independent of the mdgw adapter pipeline.
        // Group endpoints by exchange so each venue gets its own spool +
        // single-writer thread. Spool path is `{venue}-rest` so the WS
        // converter doesn't see these records.
        std::unordered_map<std::string, std::vector<refdata::EndpointSpec>>
            endpoints_per_venue;
        for (const auto& e : settings_.refdata_endpoints) {
            refdata::EndpointSpec spec;
            spec.exchange = e.exchange;
            spec.host = e.host;
            spec.port = e.port;
            spec.use_tls = e.use_tls;
            spec.method = e.method;
            spec.path = e.path;
            spec.body = e.body;
            spec.interval_seconds = e.interval_seconds;
            endpoints_per_venue[e.exchange].push_back(std::move(spec));
        }
        for (auto& [venue_name, eps] : endpoints_per_venue) {
            const std::string venue_tag = lowercase_venue(venue_name) + "-rest";
            auto spool = std::make_shared<bpt::common::recorder::RawSpool>(
                bpt::common::recorder::RawSpool::Config{
                    .root_dir = settings_.recording.output_dir,
                    .venue_tag = venue_tag,
                    .rotate_interval_seconds = settings_.recording.rotate_interval_seconds,
                    .buffer_bytes = settings_.recording.buffer_bytes,
                    .flush_interval_ns =
                        static_cast<uint64_t>(settings_.recording.fsync_interval_ms) *
                        1'000'000ULL,
                });
            const std::string snapshot = fmt::format(
                R"({{"pid":{},"exchange":"{}","kind":"refdata","endpoints":{}}})",
                ::getpid(), venue_name, eps.size());
            spool->write_marker(wall_now_ns(),
                                bpt::common::recorder::RecordType::SESSION_START,
                                snapshot);
            spool->flush();
            auto poller = std::make_unique<refdata::RefdataPoller>(
                venue_tag, spool, std::move(eps));
            poller->start();
            refdata_spools_.push_back(spool);
            refdata_pollers_.push_back(std::move(poller));
        }
    }

    void run() override {
        bpt::common::log::info("bpt-tape running — Ctrl-C to stop");
        // Block until a signal flips the running flag. Adapter threads do
        // the actual capture work; this thread just waits.
        while (bpt::common::signal::is_running()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        bpt::common::log::info("bpt-tape: signal received, stopping");
    }

    void stop() override {
        // Stop adapters first — joins their IO + publisher threads — then
        // write SESSION_STOP markers + flush each spool. Order matters:
        // the IO thread is the sole writer, so it must be quiesced before
        // we touch the spool from this thread.
        for (auto& a : adapters_) a->stop();
        for (auto& s : spools_) {
            s->write_marker(wall_now_ns(),
                            bpt::common::recorder::RecordType::SESSION_STOP,
                            R"({"reason":"stop"})");
            s->flush();
        }
        // Same dance for refdata pollers — stop() joins each poll thread,
        // so the spool is quiescent by the time we write SESSION_STOP.
        for (auto& p : refdata_pollers_) p->stop();
        for (auto& s : refdata_spools_) {
            s->write_marker(wall_now_ns(),
                            bpt::common::recorder::RecordType::SESSION_STOP,
                            R"({"reason":"stop"})");
            s->flush();
        }
    }

private:
    struct ConnState {
        bool was_disconnected{false};
        uint32_t reconnect_count{0};
    };

    config::Settings settings_;
    const bpt::common::util::Topology& topology_;
    std::vector<std::shared_ptr<bpt::common::recorder::RawSpool>> spools_;
    std::vector<std::shared_ptr<bpt::md_gateway::adapter::IAdapter>> adapters_;
    std::unordered_map<std::string,
                       std::shared_ptr<bpt::md_gateway::adapter::IAdapter>> adapters_per_venue_;
    std::vector<std::shared_ptr<bpt::common::recorder::RawSpool>> refdata_spools_;
    std::vector<std::unique_ptr<refdata::RefdataPoller>> refdata_pollers_;
};

}  // namespace

}  // namespace bpt::tape

int main(int argc, char* argv[]) {
    CLI::App cli{"bpt-tape — venue WS-frame recorder"};
    std::string config_path;
    cli.add_option("-c,--config", config_path, "Path to TOML config file")
        ->required()
        ->check(CLI::ExistingFile);
    CLI11_PARSE(cli, argc, argv);

    bpt::tape::config::Settings cfg;
    try {
        cfg = bpt::tape::config::load(config_path);
    } catch (const std::exception& e) {
        bpt::common::logging::init("bpt-tape");
        bpt::common::log::error("Failed to load config: {}", e.what());
        return 1;
    }

    try {
        // Recording host runs no Aeron consumers — venue WS frames go
        // straight to disk via RawSpool, no SBE published, no refdata
        // catalog subscription (universe loaded directly from JSON).
        // Skip the MediaDriver connect; the recording host needs zero
        // Aeron infrastructure.
        return bpt::app::run("bpt-tape", std::move(cfg),
            [](auto& settings, auto& ctx) -> std::unique_ptr<bpt::app::IService> {
                return std::make_unique<bpt::tape::RecorderService>(
                    std::move(settings), ctx.topology);
            },
            bpt::app::RunOptions{.connect_aeron = false});
    } catch (const std::exception& e) {
        bpt::common::log::error("Fatal: {}", e.what());
        return 1;
    }
}
