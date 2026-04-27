#include "md_gateway/adapter/common/adapter_base.h"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <string>
#include <thread>
#include <bpt_common/logging.h>
#include <bpt_common/util/thread_name.h>
#include <bpt_common/util/thread_pin.h>

namespace bpt::md_gateway::adapter {

namespace {

std::string lowercase_venue(const char* exchange) {
    std::string venue = exchange;
    std::transform(venue.begin(), venue.end(), venue.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return venue;
}

// Compose the topology role name used by md-gateway IO threads:
// "mdgw.<venue-lower>.io". Keeps the role vocabulary in sync with
// the service_name abbreviation used elsewhere (bpt-mdgw-<venue>).
std::string io_role(const char* exchange) {
    return "mdgw." + lowercase_venue(exchange) + ".io";
}

// OS thread names for the two AdapterBase threads. Venue in the middle
// so sort order groups all threads of the same venue together in ps -L
// (mdgw-okx-io, mdgw-okx-log, mdgw-okx-pub sit adjacent alphabetically).
// Matches the existing quill-backend (mdgw-<venue>-log) and topology-role
// (mdgw.<venue>.<subsystem>) ordering. 15-char cap per Linux TASK_COMM_LEN.
std::string io_thread_name(const char* exchange) {
    return "mdgw-" + lowercase_venue(exchange) + "-io";
}
std::string pub_thread_name(const char* exchange) {
    return "mdgw-" + lowercase_venue(exchange) + "-pub";
}

}  // namespace

namespace ssl = boost::asio::ssl;

AdapterBase::AdapterBase(const config::AdapterConfig& cfg,
                         std::shared_ptr<messaging::IMdPublisher> md_pub,
                         const config::RecordingConfig& recording)
    : cfg_(cfg),
      md_pub_(std::move(md_pub)),
      validator_(cfg.max_price_deviation_pct),
      validating_pub_(*md_pub_, validator_, cfg_.exchange.c_str()),
      ssl_ctx_(ssl::context::tls_client) {
    ssl_ctx_.set_default_verify_paths();
    ssl_ctx_.set_verify_mode(ssl::verify_peer);
    // Enforce TLS 1.2 minimum — disable weak protocol versions.
    ssl_ctx_.set_options(ssl::context::no_tlsv1 | ssl::context::no_tlsv1_1);

    // Apply validation-drop breaker config from the adapter's TOML block.
    // Default-constructed Config is disabled, so adapters that don't set
    // the knobs behave identically to before this change.
    md::ValidationDropBreaker::Config db_cfg;
    db_cfg.enabled = cfg_.validation_drop_breaker_enabled;
    db_cfg.threshold_pct = cfg_.validation_drop_threshold_pct;
    db_cfg.window_ns = static_cast<uint64_t>(cfg_.validation_drop_window_sec) * 1'000'000'000ULL;
    db_cfg.min_events = cfg_.validation_drop_min_events;
    validating_pub_.set_drop_breaker_config(db_cfg);

    if (recording.enabled) {
        recorder::RawSpool::Config sc{
            .root_dir = recording.output_dir,
            .venue_tag = lowercase_venue(cfg_.exchange.c_str()),
            .rotate_interval_seconds = recording.rotate_interval_seconds,
            .buffer_bytes = recording.buffer_bytes,
            .flush_interval_ns = static_cast<uint64_t>(recording.fsync_interval_ms) * 1'000'000ULL,
        };
        spool_ = std::make_unique<recorder::RawSpool>(std::move(sc));
        checkpoint_interval_ns_ =
            static_cast<uint64_t>(recording.checkpoint_interval_seconds) * 1'000'000'000ULL;

        // SESSION_START with config snapshot — pid, exchange, ws endpoint.
        // The recv_ts is wall-clock so converters can match it to audit log.
        const uint64_t now_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        const std::string snapshot = fmt::format(
            R"({{"pid":{},"exchange":"{}","ws":"{}://{}:{}{}"}})",
            ::getpid(), cfg_.exchange,
            cfg_.use_tls ? "wss" : "ws",
            cfg_.ws_host, cfg_.ws_port, cfg_.ws_path);
        spool_->write_marker(now_ns, recorder::RecordType::SESSION_START, snapshot);
        spool_->flush();
        last_checkpoint_ns_ = now_ns;

        bpt::common::log::info("Raw recording enabled for {} → {}",
                               cfg_.exchange, spool_->current_path());
    }
}

void AdapterBase::record_raw(std::string_view payload, uint64_t recv_ns) noexcept {
    if (spool_)
        spool_->write_frame(recv_ns, payload);
}

void AdapterBase::maybe_checkpoint() noexcept {
    if (!spool_ || checkpoint_interval_ns_ == 0)
        return;
    const uint64_t now_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    if (now_ns - last_checkpoint_ns_ < checkpoint_interval_ns_)
        return;
    last_checkpoint_ns_ = now_ns;
    const std::string payload = fmt::format(
        R"({{"frames":{},"bytes":{}}})",
        spool_->frames_written(), spool_->bytes_written());
    spool_->write_marker(now_ns, recorder::RecordType::CHECKPOINT, payload);
    spool_->flush();  // checkpoint flush bounds replay-loss to ≤ checkpoint interval
}

void AdapterBase::subscribe(uint64_t instrument_id, std::string symbol, uint8_t depth) {
    subs_.subscribe(instrument_id, std::move(symbol), depth);
}

void AdapterBase::unsubscribe(uint64_t instrument_id) {
    subs_.unsubscribe(instrument_id);
}

void AdapterBase::start() {
    pub_thread_ = std::thread([this]() { publish_loop(); });
    thread_ = std::thread([this]() { run(); });
}

void AdapterBase::stop() {
    stop_flag_.store(true, std::memory_order_relaxed);
    ioc_.stop();
    if (thread_.joinable())
        thread_.join();
    if (pub_thread_.joinable())
        pub_thread_.join();

    // IO thread is now joined — safe to write a final SESSION_STOP without
    // racing the on_frame writer. Done here (not in destructor) because
    // process exit may not run destructors under SIGKILL or std::abort.
    if (spool_) {
        const uint64_t now_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        const std::string payload = R"({"reason":"stop"})";
        spool_->write_marker(now_ns, recorder::RecordType::SESSION_STOP, payload);
        spool_->flush();
    }
}

std::chrono::milliseconds AdapterBase::reconnect_delay() const {
    return std::chrono::seconds(1);
}

void AdapterBase::push_frame(std::string_view payload, uint64_t recv_ns) noexcept {
    if (!frame_queue_.try_push(recv_ns, payload)) {
        ++dropped_frames_;
        // Log at most once every 1000 drops to avoid flooding on sustained backpressure.
        if (dropped_frames_ == 1 || dropped_frames_ % 1000 == 0) {
            bpt::common::log::warn("{}: frame queue full or oversized — dropped frames: {}", exchange_name(), dropped_frames_);
        }
    }
}

void AdapterBase::publish_loop() {
    bpt::common::util::set_thread_name(pub_thread_name(exchange_name()));
    while (!stop_flag_.load(std::memory_order_relaxed)) {
        bool processed =
            frame_queue_.try_pop([this](uint64_t recv_ns, std::string_view payload) { parse_frame(payload, recv_ns); });
        if (!processed)
            std::this_thread::yield();
    }
    // Drain any frames queued between the IO thread stopping and publish_loop waking.
    while (
        frame_queue_.try_pop([this](uint64_t recv_ns, std::string_view payload) { parse_frame(payload, recv_ns); })) {
    }
}

void AdapterBase::run() {
    bpt::common::util::set_thread_name(io_thread_name(exchange_name()));
    // Pin policy: prefer central Topology role assignment when set;
    // fall back to the legacy per-adapter cfg_.io_thread_cpu knob for
    // configs that haven't migrated. Both unset = unpinned.
    bool pinned_via_topology = false;
    if (topology_)
        pinned_via_topology = bpt::common::util::pin_thread_by_role(
            *topology_, io_role(exchange_name()), exchange_name());
    if (!pinned_via_topology)
        bpt::common::util::pin_thread_to_cpu(cfg_.io_thread_cpu, exchange_name());
    while (!stop_flag_.load(std::memory_order_relaxed)) {
        try {
            ioc_.restart();
            validator_.reset();
            auto ws = connect_and_subscribe();
            if (!ws) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            if (on_connect)
                on_connect();
            read_loop(*ws);  // NOLINT(bugprone-unchecked-optional-access)
        } catch (const std::exception& e) {
            if (!stop_flag_.load(std::memory_order_relaxed)) {
                if (on_disconnect)
                    on_disconnect();
                bpt::common::log::error("{} error: {}, reconnecting in {}ms",
                                exchange_name(),
                                e.what(),
                                reconnect_delay().count());
                std::this_thread::sleep_for(reconnect_delay());
            }
        }
    }
}

}  // namespace bpt::md_gateway::adapter
