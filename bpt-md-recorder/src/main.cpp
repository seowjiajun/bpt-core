/// bpt-md-recorder — captures venue WS frames to disk by importing
/// bpt-md-gateway's adapter library and substituting a recording subclass
/// that tees raw bytes via bpt::common::recorder::RawSpool.
///
/// mdgw and refdata service binaries are unchanged; this process owns the
/// recording feature in full. No Aeron data publication; a no-op publisher
/// satisfies the adapter's IMdPublisher dependency so parsing still runs
/// (cheap on the recording host) and the wire pipeline behaves identically
/// to live trading at every layer except the disk tap.

#include "md_recorder/adapter/recording_mdgw_adapters.h"
#include "md_recorder/config/settings.h"
#include "bpt_common/recorder/raw_spool.h"
#include "md_gateway/messaging/i_md_publisher.h"
#include "md_gateway/adapter/common/i_adapter.h"

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

namespace bpt::md_recorder {

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
class NoopMdPublisher : public bpt::md_gateway::messaging::IMdPublisher {
public:
    void publish(const bpt::md_gateway::md::MdBbo&) override {}
    void publish(const bpt::md_gateway::md::MdTrade&) override {}
    void publish(const bpt::md_gateway::md::MdOrderBook&) override {}
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

            std::shared_ptr<bpt::md_gateway::adapter::IAdapter> adapter;
            if (a_cfg.exchange == "BINANCE")
                adapter = std::make_shared<adapter::RecordingBinanceAdapter>(spool, a_cfg, pub);
            else if (a_cfg.exchange == "OKX")
                adapter = std::make_shared<adapter::RecordingOkxAdapter>(spool, a_cfg, pub);
            else if (a_cfg.exchange == "HYPERLIQUID")
                adapter = std::make_shared<adapter::RecordingHyperliquidAdapter>(spool, a_cfg, pub);
            else if (a_cfg.exchange == "DERIBIT")
                adapter = std::make_shared<adapter::RecordingDeribitAdapter>(spool, a_cfg, pub);
            else {
                bpt::common::log::warn("Unknown exchange in config: {}", a_cfg.exchange);
                continue;
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
            bpt::common::log::info("md-recorder: started recording adapter for {} → {}",
                                   a_cfg.exchange, spool->current_path());
        }

        // Apply universe — each (venue, symbol, instrument_id) becomes a
        // subscribe call on the matching adapter. Adapters sit in a
        // pre-connect state until subscribe lands the first symbol; first
        // subscribe drives connect_and_subscribe on the IO thread.
        size_t n_subscribed = 0;
        for (const auto& u : settings_.universe) {
            auto it = adapters_per_venue_.find(u.venue);
            if (it == adapters_per_venue_.end()) {
                bpt::common::log::warn("Universe entry venue '{}' has no matching adapter — skipping {}",
                                       u.venue, u.symbol);
                continue;
            }
            it->second->subscribe(u.instrument_id, u.symbol, u.depth);
            ++n_subscribed;
        }
        bpt::common::log::info("md-recorder: subscribed {} symbols across {} adapters",
                               n_subscribed, adapters_.size());
    }

    void run() override {
        bpt::common::log::info("md-recorder running — Ctrl-C to stop");
        // Block until a signal flips the running flag. Adapter threads do
        // the actual capture work; this thread just waits.
        while (bpt::common::signal::is_running()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        bpt::common::log::info("md-recorder: signal received, stopping");
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
};

}  // namespace

}  // namespace bpt::md_recorder

int main(int argc, char* argv[]) {
    CLI::App cli{"bpt-md-recorder — venue WS-frame recorder"};
    std::string config_path;
    cli.add_option("-c,--config", config_path, "Path to TOML config file")
        ->required()
        ->check(CLI::ExistingFile);
    CLI11_PARSE(cli, argc, argv);

    bpt::md_recorder::config::Settings cfg;
    try {
        cfg = bpt::md_recorder::config::load(config_path);
    } catch (const std::exception& e) {
        bpt::common::logging::init("bpt-md-recorder");
        bpt::common::log::error("Failed to load config: {}", e.what());
        return 1;
    }

    try {
        return bpt::app::run("bpt-md-recorder", std::move(cfg),
            [](auto& settings, auto& ctx) -> std::unique_ptr<bpt::app::IService> {
                return std::make_unique<bpt::md_recorder::RecorderService>(
                    std::move(settings), ctx.topology);
            });
    } catch (const std::exception& e) {
        bpt::common::log::error("Fatal: {}", e.what());
        return 1;
    }
}
