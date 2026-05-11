/// \file
/// \brief RecorderService implementation — see header for contract.

#include "tape/app/recorder_service.h"

#include "tape/adapter/recording_mdgw_adapters.h"
#include "tape/io/tape.h"
#include "md_gateway/md/md_types.h"
#include "refdata/mapping/instrument_mapping_loader.h"
#include <messages/ExchangeRegistry.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fmt/format.h>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <bpt_common/logging.h>
#include <bpt_common/signal.h>
#include <bpt_common/util/tsc_clock.h>

namespace bpt::tape::app {

using bpt::common::util::WallClock;

namespace {

std::string lowercase_venue(const std::string& exchange) {
    std::string out = exchange;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}

// Discards all parsed SBE — recorder has no downstream consumers, the
// disk tap on raw frames is the only output. Templated venue adapters
// inline these no-ops; the publish() chain compiles to dead branches.
class NoopMdPublisher {
public:
    void publish(const bpt::md_gateway::md::MdBbo&) {}
    void publish(const bpt::md_gateway::md::MdTrade&) {}
    void publish(const bpt::md_gateway::md::MdOrderBook&) {}
    uint64_t drop_count() const { return 0; }
};

}  // namespace

RecorderService::RecorderService(config::Settings settings,
                                 const bpt::common::util::Topology& topology)
    : settings_(std::move(settings)), topology_(topology) {
    setup_metrics();
    setup_mdgw_recording();
    setup_universe();
    setup_refdata_pollers();
}

void RecorderService::setup_metrics() {
    // metrics_port == 0 disables the endpoint (BaseSettings default).
    // Useful for local dev runs that don't want a port collision.
    if (settings_.base.metrics_port == 0) return;

    metrics_ = std::make_unique<metrics::TapeMetrics>(
        settings_.metrics_host, settings_.base.metrics_port);
    bpt::common::log::info("bpt-tape: metrics exposer at {}:{}",
                           settings_.metrics_host,
                           settings_.base.metrics_port);
}

std::shared_ptr<bpt::tape::io::Tape>
RecorderService::make_tape(const std::string& venue_tag) {
    return std::make_shared<bpt::tape::io::Tape>(
        bpt::tape::io::Tape::Config{
            .root_dir = settings_.recording.output_dir,
            .venue_tag = venue_tag,
            .rotate_interval_seconds = settings_.recording.rotate_interval_seconds,
            .buffer_bytes = settings_.recording.buffer_bytes,
            .flush_interval_ns =
                static_cast<uint64_t>(settings_.recording.fsync_interval_ms) *
                1'000'000ULL,
            .metrics = metrics_ ? metrics_->hooks_for(venue_tag)
                                : bpt::tape::io::Tape::MetricsHooks{},
        });
}

void RecorderService::wire_connection_markers(
    std::shared_ptr<bpt::md_gateway::adapter::IAdapter> adapter,
    std::shared_ptr<bpt::tape::io::Tape> tape,
    const std::string& venue_tag) {
    // ConnState shared via shared_ptr so both lambdas see the same
    // counter + "have we ever disconnected" flag. metrics_ raw pointer
    // is safe — TapeMetrics outlives all adapters by member order.
    auto state = std::make_shared<ConnState>();
    auto* metrics = metrics_.get();

    adapter->on_connect = [tape, state, metrics, venue_tag]() {
        if (metrics) metrics->on_ws_connect(venue_tag);
        if (!state->was_disconnected) return;  // initial connect — SESSION_START covers it
        ++state->reconnect_count;
        tape->write_marker(WallClock::now_ns(),
                            bpt::common::recorder::RecordType::WS_RECONNECT,
                            fmt::format(R"({{"attempt":{}}})", state->reconnect_count));
        tape->flush();
        state->was_disconnected = false;
    };
    adapter->on_disconnect = [tape, state, metrics, venue_tag]() {
        if (metrics) metrics->on_ws_disconnect(venue_tag);
        tape->write_marker(WallClock::now_ns(),
                            bpt::common::recorder::RecordType::WS_DISCONNECT,
                            fmt::format(R"({{"attempt":{}}})",
                                        state->reconnect_count + 1));
        tape->flush();
        state->was_disconnected = true;
    };
}

void RecorderService::setup_mdgw_recording() {
    auto pub = std::make_shared<NoopMdPublisher>();

    for (const auto& a_cfg : settings_.mdgw_adapters) {
        const std::string venue_tag = lowercase_venue(a_cfg.exchange);
        auto tape = make_tape(venue_tag);

        tape->write_marker(WallClock::now_ns(),
                            bpt::common::recorder::RecordType::SESSION_START,
                            fmt::format(R"({{"pid":{},"exchange":"{}","ws":"{}://{}:{}{}"}})",
                                        ::getpid(), a_cfg.exchange,
                                        a_cfg.use_tls ? "wss" : "ws",
                                        a_cfg.ws_host, a_cfg.ws_port, a_cfg.ws_path));
        tape->flush();

        const auto exch_id = bpt::messages::ExchangeRegistry::from_name(a_cfg.exchange);
        if (!exch_id) {
            throw std::runtime_error(fmt::format(
                "Unknown exchange '{}' in bpt-tape config — not in messages/exchanges.yaml",
                a_cfg.exchange));
        }
        auto adapter = adapter::make_recording_adapter<NoopMdPublisher>(
            *exch_id, tape, a_cfg, pub);
        if (!adapter) {
            throw std::runtime_error(fmt::format(
                "Exchange '{}' is in the registry but bpt-tape has no recording adapter for it",
                a_cfg.exchange));
        }

        wire_connection_markers(adapter, tape, venue_tag);

        adapter->set_topology(topology_);
        adapter->start();
        tapes_.push_back(tape);
        adapters_per_venue_[a_cfg.exchange] = adapter;
        adapters_.push_back(std::move(adapter));
        bpt::common::log::info("bpt-tape: started recording adapter for {} → {}",
                               a_cfg.exchange, tape->current_path());
    }
}

void RecorderService::setup_universe() {
    // Throws here would orphan the adapter threads started in
    // setup_mdgw_recording (AdapterBase has no joining destructor — stop()
    // is the lifecycle handle). Stop them before re-throwing.
    try {
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

        size_t n_subscribed_total = 0;
        for (const auto& [venue_name, adapter] : adapters_per_venue_) {
            const auto venue_id = bpt::messages::ExchangeRegistry::from_name(venue_name);
            if (!venue_id) continue;  // unreachable: ctor validates above
            const auto entries = mapping.instruments_for_venue(static_cast<uint8_t>(*venue_id));
            std::size_t n_for_this_venue = 0;
            for (const auto& e : entries) {
                if (!matches_filter(e)) continue;
                adapter->subscribe(e.canonical_id, e.venue_symbol, filter.default_depth);
                ++n_for_this_venue;
            }
            n_subscribed_total += n_for_this_venue;
            // Lowercase to match the venue-tag label used by other
            // metrics (frames_written_total, etc.).
            if (metrics_) {
                metrics_->set_subscriptions(lowercase_venue(venue_name), n_for_this_venue);
            }
        }
        bpt::common::log::info("bpt-tape: subscribed {} symbols across {} adapters",
                               n_subscribed_total, adapters_.size());
    } catch (...) {
        for (auto& a : adapters_) a->stop();
        throw;
    }
}

void RecorderService::setup_refdata_pollers() {
    // Group by venue so each gets its own tape + single-writer thread.
    // Spool path suffix `-rest` keeps these records out of the WS
    // converter's input.
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
        auto tape = make_tape(venue_tag);
        tape->write_marker(WallClock::now_ns(),
                            bpt::common::recorder::RecordType::SESSION_START,
                            fmt::format(R"({{"pid":{},"exchange":"{}","kind":"refdata","endpoints":{}}})",
                                        ::getpid(), venue_name, eps.size()));
        tape->flush();
        auto poller = std::make_unique<refdata::RefdataPoller>(
            venue_tag, tape, std::move(eps));
        poller->start();
        refdata_tapes_.push_back(tape);
        refdata_pollers_.push_back(std::move(poller));
    }
}

void RecorderService::run() {
    bpt::common::log::info("bpt-tape running — Ctrl-C to stop");
    while (bpt::common::signal::is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    bpt::common::log::info("bpt-tape: signal received, stopping");
}

void RecorderService::stop() {
    // Stop the IO thread (sole tape writer) BEFORE touching the tape
    // from this thread — otherwise SESSION_STOP races with a frame write.
    for (auto& a : adapters_) a->stop();
    for (auto& s : tapes_) {
        s->write_marker(WallClock::now_ns(),
                        bpt::common::recorder::RecordType::SESSION_STOP,
                        R"({"reason":"stop"})");
        s->flush();
    }
    for (auto& p : refdata_pollers_) p->stop();
    for (auto& s : refdata_tapes_) {
        s->write_marker(WallClock::now_ns(),
                        bpt::common::recorder::RecordType::SESSION_STOP,
                        R"({"reason":"stop"})");
        s->flush();
    }
    // healthy=0 last — dashboards key on it being 1-then-0 (clean stop)
    // vs scrape-just-vanishes (crash).
    if (metrics_) metrics_->shutdown();
}

}  // namespace bpt::tape::app
