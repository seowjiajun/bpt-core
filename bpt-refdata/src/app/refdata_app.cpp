#include "refdata/app/refdata_app.h"

#include "refdata/adapter/binance/binance_refdata_adapter.h"
#include "refdata/adapter/deribit/deribit_refdata_adapter.h"
#include "refdata/adapter/hyperliquid/hyperliquid_refdata_adapter.h"
#include "refdata/adapter/okx/okx_refdata_adapter.h"

#include <messages/DeltaUpdateType.h>
#include <messages/ExchangeId.h>
#include <messages/ExchangeRegistry.h>
#include <messages/RefDataErrorType.h>

#include <chrono>
#include <future>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <bpt_common/signal.h>

using namespace std::chrono_literals;

namespace bpt::refdata {

namespace {

uint8_t exchange_id_to_bit(bpt::messages::ExchangeId::Value id) {
    switch (id) {
        case bpt::messages::ExchangeId::BINANCE:
            return 0x01;
        case bpt::messages::ExchangeId::OKX:
            return 0x02;
        case bpt::messages::ExchangeId::HYPERLIQUID:
            return 0x04;
        case bpt::messages::ExchangeId::DERIBIT:
            return 0x08;
        default:
            return 0x00;
    }
}

}  // namespace

RefdataApp::RefdataApp(config::Settings settings,
                     std::shared_ptr<aeron::Aeron> aeron,
                     std::map<std::string, adapter::ExchangeCredentials> creds)
    : settings_(std::move(settings)),
      aeron_(aeron),
      metrics_(settings_.base.metrics_port),
      instrument_mapping_(std::make_shared<mapping::InstrumentMappingLoader>()),
      registry_(std::make_shared<registry::InstrumentRegistry>()) {
    const auto& im_cfg = settings_.instrument_mapping;

    // If per-exchange sources are configured, merge them into the canonical
    // file now. Falls back to whatever's already at local_path if the merge
    // fails (e.g. running from a stale deploy with no new sources).
    if (!im_cfg.sources.paths.empty()) {
        mapping_merger_.emplace(mapping::InstrumentMappingMerger::Config{im_cfg.sources.paths});
        if (!mapping_merger_->merge(im_cfg.local_path))
            bpt::common::log::warn("Merge failed — attempting to load cached local file");
    }

    instrument_mapping_->load(im_cfg.local_path);

    control_sub_ = std::make_unique<messaging::RefdataControlSubscriber>(aeron,
                                                                         settings_.control.channel,
                                                                         settings_.control.stream_id);
    snapshot_pub_ = std::make_unique<messaging::RefdataSnapshotPublisher>(aeron,
                                                                          settings_.snapshot.channel,
                                                                          settings_.snapshot.stream_id);
    delta_pub_ =
        std::make_shared<messaging::RefdataDeltaPublisher>(aeron, settings_.delta.channel, settings_.delta.stream_id);
    fee_pub_ = std::make_shared<messaging::FeeSchedulePublisher>(aeron,
                                                                 settings_.fee_schedule.channel,
                                                                 settings_.fee_schedule.stream_id);
    status_pub_ = std::make_shared<messaging::RefdataStatusPublisher>(aeron,
                                                                     settings_.refdata_status.channel,
                                                                     settings_.refdata_status.stream_id);

    for (const auto& cfg : settings_.adapters) {
        const auto& exchange_creds = [&]() -> const adapter::ExchangeCredentials& {
            static const adapter::ExchangeCredentials empty{};
            const auto it = creds.find(cfg.exchange);
            return it != creds.end() ? it->second : empty;
        }();

        // Build the RestClient(s) here so a recording-aware subclass can be
        // substituted at this construction site without touching adapter code.
        // Per-venue host fallback lives here too — adapters consume the client
        // pre-configured.
        auto make_client = [&cfg](const std::string& default_host) {
            const std::string host = cfg.rest_host.empty() ? default_host : cfg.rest_host;
            return std::make_shared<http::RestClient>(host, cfg.rest_port, cfg.use_tls);
        };

        // Validate at the boundary via ExchangeRegistry — typo in TOML
        // throws here rather than silently skipping with a logged error
        // (which previously meant catalog instruments for that venue
        // never published, with no obvious cause beyond the log line).
        const auto exch_id = bpt::messages::ExchangeRegistry::from_name(cfg.exchange);
        if (!exch_id) {
            throw std::runtime_error(fmt::format(
                "Unknown exchange '{}' in refdata config — not in messages/exchanges.yaml",
                cfg.exchange));
        }

        std::unique_ptr<adapter::IExchangeRefDataAdapter> adapter;
        switch (*exch_id) {
            case bpt::messages::ExchangeId::BINANCE:
                adapter = std::make_unique<adapter::BinanceRefDataAdapter>(
                    cfg, exchange_creds, registry_, instrument_mapping_,
                    make_client("api.binance.com"),
                    make_client("fapi.binance.com"));
                break;
            case bpt::messages::ExchangeId::OKX:
                adapter = std::make_unique<adapter::OKXRefDataAdapter>(
                    cfg, exchange_creds, registry_, instrument_mapping_,
                    make_client("www.okx.com"));
                break;
            case bpt::messages::ExchangeId::HYPERLIQUID:
                adapter = std::make_unique<adapter::HyperliquidRefDataAdapter>(
                    cfg, exchange_creds, registry_, instrument_mapping_,
                    make_client("api.hyperliquid.xyz"));
                break;
            case bpt::messages::ExchangeId::DERIBIT:
                adapter = std::make_unique<adapter::DeribitRefDataAdapter>(
                    cfg, exchange_creds, registry_, instrument_mapping_,
                    make_client("test.deribit.com"));
                break;
            default:
                throw std::runtime_error(fmt::format(
                    "Exchange '{}' is in the registry but refdata has no adapter implementation for it",
                    cfg.exchange));
        }

        const std::string exch_name = adapter->exchange_name();
        adapter->on_fee_schedule = [this, exch_name](const refdata::FeeScheduleState& fs) {
            std::lock_guard lock(pub_mutex_);
            fee_pub_->publish(fs);
            metrics_.fee_update(exch_name).Increment();
        };
        adapter->on_instrument_delta = [this](const refdata::Instrument& inst,
                                              bpt::messages::DeltaUpdateType::Value update_type,
                                              uint64_t /*collected_ts_ns*/) {
            delta_pub_->publish_delta(update_type, inst);
            metrics_.last_update_ns->Set(static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()));
        };

        adapters_.push_back(std::move(adapter));
    }

    if (adapters_.empty())
        throw std::runtime_error("No adapters configured — nothing to serve.");

    bpt::common::log::info("Ready — listening on {} stream {}",
                   settings_.control.channel,
                   settings_.control.stream_id);
}

void RefdataApp::run() {
    // Parallel snapshot fetch — all adapters run concurrently; registry is thread-safe.
    // fee_pub_ is guarded by pub_mutex_ in the on_fee_schedule callback.
    uint8_t exchanges_loaded = 0;
    bool fee_schedules_loaded = false;

    {
        std::vector<std::future<void>> futures;
        futures.reserve(adapters_.size());
        for (auto& adapter : adapters_) {
            bpt::common::log::info("Fetching snapshot for {}...", adapter->exchange_name());
            futures.push_back(std::async(std::launch::async, [&adapter]() { adapter->fetchSnapshot(); }));
        }
        for (std::size_t i = 0; i < adapters_.size(); ++i) {
            try {
                futures[i].get();
                exchanges_loaded |= exchange_id_to_bit(adapters_[i]->exchange_id());
                fee_schedules_loaded = true;
                metrics_.exchange_ready(adapters_[i]->exchange_name()).Set(1.0);
                bpt::common::log::info("Snapshot complete for {}", adapters_[i]->exchange_name());
            } catch (const std::exception& e) {
                bpt::common::log::error("Snapshot failed for {}: {}", adapters_[i]->exchange_name(), e.what());
                status_pub_->publish_error(bpt::messages::RefDataErrorType::SNAPSHOT_FAILED,
                                           adapters_[i]->exchange_id());
                metrics_.snapshot_failure(adapters_[i]->exchange_name()).Increment();
            }
        }
    }

    for (auto& adapter : adapters_) {
        if (adapter->isReady()) {
            adapter->subscribeDeltas();
            bpt::common::log::info("Delta subscriptions started for {}", adapter->exchange_name());
        }
    }

    {
        uint16_t instrument_count = static_cast<uint16_t>(registry_->count());
        status_pub_->publish_ready(exchanges_loaded, instrument_count, fee_schedules_loaded);
    }

    constexpr auto idle_sleep = std::chrono::microseconds(10);
    constexpr auto heartbeat_interval = std::chrono::seconds(5);
    constexpr auto refdata_ready_interval = std::chrono::seconds(30);
    constexpr auto snapshot_republish_interval = std::chrono::seconds(30);
    constexpr auto mapping_refresh_interval = std::chrono::hours(24);
    const auto poll_interval = std::chrono::seconds(settings_.instrument_poll_interval_s);

    auto last_heartbeat = std::chrono::steady_clock::now();
    auto last_refdata_ready = std::chrono::steady_clock::now();
    auto last_snapshot_republish = std::chrono::steady_clock::now();
    auto last_listing_poll = std::chrono::steady_clock::now();
    auto last_mapping_refresh = std::chrono::steady_clock::now();

    while (bpt::common::signal::is_running()) {
        int fragments = control_sub_->poll([this](const messaging::RefdataRequest& request) {
            bpt::common::log::info("Request correlation_id={} filters={}",
                           request.correlation_id,
                           request.instruments.size());
            sub_manager_.upsert(request);
            snapshot_pub_->publish(*registry_, request, delta_pub_->current_sequence());
            metrics_.requests_served_total->Increment();
            metrics_.last_update_ns->Set(static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()));
        });

        auto now = std::chrono::steady_clock::now();

        if (now - last_heartbeat >= heartbeat_interval) {
            delta_pub_->publish_heartbeat();
            metrics_.instruments_total->Set(static_cast<double>(registry_->count()));
            last_heartbeat = now;
        }

        if (now - last_refdata_ready >= refdata_ready_interval) {
            uint16_t current_count = static_cast<uint16_t>(registry_->count());
            status_pub_->publish_ready(exchanges_loaded, current_count, fee_schedules_loaded);
            last_refdata_ready = now;
        }

        // Periodic snapshot republish — broadcasts the full instrument
        // catalog with an empty filter (match-all) so late-joining
        // subscribers (e.g. pricer) receive it without needing to send
        // their own subscription request.
        if (now - last_snapshot_republish >= snapshot_republish_interval) {
            messaging::RefdataRequest empty_req{};
            snapshot_pub_->publish(*registry_, empty_req, delta_pub_->current_sequence());
            metrics_.last_update_ns->Set(static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()));
            bpt::common::log::info("Periodic snapshot republish: {} instruments", registry_->count());
            last_snapshot_republish = now;
        }

        if (mapping_merger_ && now - last_mapping_refresh >= mapping_refresh_interval) {
            const auto& local = settings_.instrument_mapping.local_path;
            if (mapping_merger_->merge(local)) {
                try {
                    instrument_mapping_->load(local);
                    std::size_t delta_count = 0;
                    registry_->for_each([this, &delta_count](const refdata::Instrument& inst) {
                        delta_pub_->publish_delta(bpt::messages::DeltaUpdateType::MODIFY, inst);
                        ++delta_count;
                    });
                    bpt::common::log::info("Republished {} instrument deltas after mapping refresh", delta_count);
                } catch (const std::exception& e) {
                    bpt::common::log::error("Mapping reload failed after refresh: {}", e.what());
                }
            }
            last_mapping_refresh = now;
        }

        if (now - last_listing_poll >= poll_interval) {
            for (auto& adapter : adapters_) {
                if (!adapter->isReady())
                    continue;
                try {
                    adapter->fetchInstrumentListing();
                    metrics_.listing_refresh(adapter->exchange_name()).Increment();
                } catch (const std::exception& e) {
                    bpt::common::log::error("Instrument listing refresh failed for {}: {}",
                                    adapter->exchange_name(),
                                    e.what());
                    status_pub_->publish_error(bpt::messages::RefDataErrorType::SNAPSHOT_FAILED,
                                               adapter->exchange_id());
                }
            }
            last_listing_poll = now;
        }

        if (fragments == 0)
            std::this_thread::sleep_for(idle_sleep);
    }
}

void RefdataApp::stop() {
    // Called by bpt::app::run() after the poll loop exits on signal.
    // Adapter REST/WS threads + Prometheus exposer drained here so
    // teardown is symmetric with the startup side-effects in run().
    for (auto& adapter : adapters_)
        adapter->stop();
    metrics_.shutdown();
}

}  // namespace bpt::refdata
