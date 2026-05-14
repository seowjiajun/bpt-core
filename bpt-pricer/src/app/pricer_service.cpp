#include "pricer/app/pricer_service.h"

#include <bpt_common/logging.h>
#include <bpt_common/signal.h>
#include <bpt_common/util/tsc_clock.h>
#include <chrono>
#include <ctime>
#include <thread>

namespace {
inline uint32_t today_yyyymmdd() noexcept {
    auto tt = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    gmtime_r(&tt, &tm);
    return static_cast<uint32_t>((tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday);
}
}  // namespace

using namespace std::chrono_literals;

namespace bpt::pricer {

using bpt::common::util::WallClock;

PricerService::PricerService(config::Settings settings, messaging::PricerBus bus)
    : settings_(std::move(settings)),
      builder_(settings_.risk_free_rate, settings_.newton_max_iterations, settings_.newton_tolerance),
      bus_(std::move(bus)) {
    bus_.md_sub->set_bbo_callback([this](uint64_t instrument_id, double bid, double ask, uint64_t timestamp_ns) {
        auto pit = perp_map_.find(instrument_id);
        if (pit != perp_map_.end()) {
            builder_.set_spot(pit->second.underlying, pit->second.exchange_id, (bid + ask) * 0.5);
            return;
        }
        builder_.on_bbo(instrument_id, bid, ask, timestamp_ns);
    });

    bus_.refdata_sub->set_on_option([this](const surface::OptionInstrument& inst) {
        builder_.add_instrument(inst);
        bpt::common::log::info("Option instrument: id={} {} {} K={} exp={}",
                               inst.instrument_id,
                               inst.underlying,
                               inst.exchange,
                               inst.strike_price,
                               inst.expiry_date);
    });

    bus_.refdata_sub->set_on_perp([this](const refdata::PerpInstrument& inst) {
        perp_map_[inst.instrument_id] = {inst.underlying, inst.exchange_id};
        bpt::common::log::info("Perp instrument registered: id={} {} {}",
                               inst.instrument_id,
                               inst.underlying,
                               inst.exchange);
    });

    bus_.refdata_sub->set_on_remove([this](uint64_t instrument_id) { builder_.remove_instrument(instrument_id); });

    bpt::common::log::info("publish_interval_ms={} risk_free_rate={:.4f}",
                           settings_.publish_interval_ms,
                           settings_.risk_free_rate);
    for (const auto& u : settings_.underlyings)
        bpt::common::log::info("underlying: {}", u);
    for (const auto& e : settings_.exchanges)
        bpt::common::log::info("exchange: {}", e);
    bpt::common::log::info("Ready — entering main loop");
}

void PricerService::run() {
    constexpr auto idle_sleep = std::chrono::microseconds(100);
    const auto publish_interval = std::chrono::milliseconds(settings_.publish_interval_ms);
    constexpr auto heartbeat_interval = std::chrono::seconds(5);
    constexpr auto ready_interval = std::chrono::seconds(30);

    auto last_publish = std::chrono::steady_clock::now();
    auto last_heartbeat = std::chrono::steady_clock::now();
    auto last_ready = std::chrono::steady_clock::now();
    uint64_t heartbeat_seq = 0;
    bool initial_ready_sent = false;

    while (bpt::common::signal::is_running()) {
        int fragments = 0;
        fragments += bus_.md_sub->poll(64);
        fragments += bus_.refdata_sub->poll(64);

        auto now = std::chrono::steady_clock::now();

        if (now - last_publish >= publish_interval) {
            const uint32_t today = today_yyyymmdd();
            auto grids = builder_.build(today);

            for (const auto& grid : grids) {
                bus_.vol_pub->publish(grid, WallClock::now_ns());
                bpt::common::log::debug("Published surface: {} {} points={}",
                                        grid.underlying,
                                        static_cast<int>(grid.exchange_id),
                                        grid.points.size());
            }

            if (!initial_ready_sent && builder_.instrument_count() > 0) {
                uint8_t exchanges_loaded = 0;
                uint32_t total_points = 0;
                for (const auto& g : grids) {
                    total_points += static_cast<uint32_t>(g.points.size());
                    using EX = bpt::messages::ExchangeId;
                    if (g.exchange_id == EX::BINANCE)
                        exchanges_loaded |= 0x01;
                    else if (g.exchange_id == EX::OKX)
                        exchanges_loaded |= 0x02;
                    else if (g.exchange_id == EX::HYPERLIQUID)
                        exchanges_loaded |= 0x04;
                    else if (g.exchange_id == EX::DERIBIT)
                        exchanges_loaded |= 0x08;
                }
                bus_.status_pub->publish_ready(WallClock::now_ns(),
                                               exchanges_loaded,
                                               static_cast<uint16_t>(grids.size()),
                                               total_points);
                initial_ready_sent = true;
            }

            last_publish = now;
        }

        if (now - last_heartbeat >= heartbeat_interval) {
            bus_.status_pub->publish_heartbeat(WallClock::now_ns(), ++heartbeat_seq);
            last_heartbeat = now;
        }

        if (initial_ready_sent && now - last_ready >= ready_interval) {
            const uint32_t today = today_yyyymmdd();
            auto grids = builder_.build(today);
            uint8_t exchanges_loaded = 0;
            uint32_t total_points = 0;
            for (const auto& g : grids) {
                total_points += static_cast<uint32_t>(g.points.size());
                using EX = bpt::messages::ExchangeId;
                if (g.exchange_id == EX::BINANCE)
                    exchanges_loaded |= 0x01;
                else if (g.exchange_id == EX::OKX)
                    exchanges_loaded |= 0x02;
                else if (g.exchange_id == EX::HYPERLIQUID)
                    exchanges_loaded |= 0x04;
                else if (g.exchange_id == EX::DERIBIT)
                    exchanges_loaded |= 0x08;
            }
            bus_.status_pub->publish_ready(WallClock::now_ns(),
                                           exchanges_loaded,
                                           static_cast<uint16_t>(grids.size()),
                                           total_points);
            last_ready = now;
        }

        if (fragments == 0)
            std::this_thread::sleep_for(idle_sleep);
    }
}

}  // namespace bpt::pricer
