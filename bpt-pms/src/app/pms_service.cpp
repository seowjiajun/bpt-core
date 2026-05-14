#include "pms/app/pms_service.h"

#include "pms/adapter/hyperliquid_balance_adapter.h"

#include <messages/ExchangeRegistry.h>

#include <bpt_common/logging.h>
#include <bpt_common/signal.h>
#include <bpt_common/util/tsc_clock.h>
#include <chrono>
#include <fmt/format.h>
#include <stdexcept>
#include <thread>

namespace bpt::pms {

namespace {

std::unique_ptr<adapter::IBalanceAdapter> make_adapter(const config::AdapterConfig& a) {
    const auto exch_id = bpt::messages::ExchangeRegistry::from_name(a.exchange);
    if (!exch_id) {
        throw std::runtime_error(
            fmt::format("Unknown exchange '{}' in bpt-pms config — not in messages/exchanges.yaml", a.exchange));
    }
    switch (*exch_id) {
        case bpt::messages::ExchangeId::HYPERLIQUID: {
            adapter::HyperliquidBalanceAdapter::Config cfg{
                .rest_host = a.rest_host,
                .rest_port = a.rest_port,
                .wallet_address = a.wallet_address,
            };
            return std::make_unique<adapter::HyperliquidBalanceAdapter>(std::move(cfg));
        }
        // OKX / Binance / Deribit adapters will slot in here as they're written.
        default:
            throw std::runtime_error(fmt::format("bpt-pms has no balance adapter for {} yet", a.exchange));
    }
}

}  // namespace

PmsService::PmsService(config::Settings settings, messaging::PmsBus bus)
    : settings_(std::move(settings)),
      bus_(std::move(bus)) {
    bpt::common::log::info("Balance publication ready: {} stream {}",
                           settings_.aeron.balance_snapshot.channel,
                           settings_.aeron.balance_snapshot.stream_id);

    for (const auto& a : settings_.pms.adapters) {
        adapters_.push_back(make_adapter(a));
        bpt::common::log::info("Balance adapter ready: {}", a.exchange);
    }
    if (adapters_.empty())
        bpt::common::log::warn("No balance adapters configured — bpt-pms will publish empty snapshots");
}

void PmsService::run() {
    const auto interval = std::chrono::milliseconds(settings_.pms.poll_interval_ms);
    bpt::common::log::info("bpt-pms poll loop starting, interval={} ms", settings_.pms.poll_interval_ms);

    while (bpt::common::signal::is_running()) {
        adapter::BalanceSnapshot snap;
        snap.correlation_id = ++correlation_id_;
        snap.timestamp_ns = bpt::common::util::WallClock::now_ns();

        for (auto& ad : adapters_) {
            try {
                auto rows = ad->fetch();
                snap.rows.insert(snap.rows.end(),
                                 std::make_move_iterator(rows.begin()),
                                 std::make_move_iterator(rows.end()));
            } catch (const std::exception& e) {
                // Log and continue. A single-venue failure must not take
                // down the whole snapshot — dashboards degrade per-venue
                // gracefully. Next tick will retry.
                bpt::common::log::warn("{} balance fetch failed: {}", ad->venue_name(), e.what());
            }
        }

        bus_.snapshot_pub->publish(snap);

        // Sleep in small slices so shutdown signals are noticed promptly
        // instead of being gated by a 5-second poll interval.
        const auto slice = std::chrono::milliseconds(200);
        auto slept = std::chrono::milliseconds(0);
        while (bpt::common::signal::is_running() && slept < interval) {
            std::this_thread::sleep_for(std::min(slice, interval - slept));
            slept += slice;
        }
    }

    bpt::common::log::info("bpt-pms poll loop exiting");
}

}  // namespace bpt::pms
