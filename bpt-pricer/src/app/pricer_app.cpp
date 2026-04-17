#include "pricer/app/pricer_app.h"

#include <chrono>
#include <ctime>
#include <thread>
#include <yggdrasil/logging.h>
#include <yggdrasil/signal.h>

namespace {
inline uint64_t now_ns() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}
inline uint32_t today_yyyymmdd() noexcept {
    auto tt = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    gmtime_r(&tt, &tm);
    return static_cast<uint32_t>((tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday);
}
}  // namespace

using namespace std::chrono_literals;

namespace bpt::pricer {

PricerApp::PricerApp(config::Settings settings, std::shared_ptr<aeron::Aeron> aeron)
    : settings_(std::move(settings)),
      builder_(settings_.risk_free_rate, settings_.newton_max_iterations, settings_.newton_tolerance) {
    vol_pub_ = std::make_unique<messaging::VolSurfacePublisher>(aeron,
                                                                settings_.vol_surface.channel,
                                                                settings_.vol_surface.stream_id,
                                                                settings_.pub_timeout_ms,
                                                                settings_.pub_poll_interval_ms);
    status_pub_ = std::make_unique<messaging::StatusPublisher>(aeron,
                                                               settings_.status.channel,
                                                               settings_.status.stream_id,
                                                               settings_.pub_timeout_ms,
                                                               settings_.pub_poll_interval_ms);
    md_sub_ = std::make_unique<md::MdSubscriber>(aeron, settings_.md_input.channel, settings_.md_input.stream_id);
    refdata_sub_ = std::make_unique<refdata::RefdataSubscriber>(aeron,
                                                                settings_.refdata_snapshot.channel,
                                                                settings_.refdata_snapshot.stream_id,
                                                                settings_.refdata_delta.channel,
                                                                settings_.refdata_delta.stream_id,
                                                                settings_.refdata_control.channel,
                                                                settings_.refdata_control.stream_id);

    md_sub_->set_bbo_callback([this](uint64_t instrument_id, double bid, double ask, uint64_t timestamp_ns) {
        auto pit = perp_map_.find(instrument_id);
        if (pit != perp_map_.end()) {
            builder_.set_spot(pit->second.underlying, pit->second.exchange_id, (bid + ask) * 0.5);
            return;
        }
        builder_.on_bbo(instrument_id, bid, ask, timestamp_ns);
    });

    refdata_sub_->set_on_option([this](const surface::OptionInstrument& inst) {
        builder_.add_instrument(inst);
        ygg::log::info("[Pricer] Option instrument: id={} {} {} K={} exp={}",
                       inst.instrument_id,
                       inst.underlying,
                       inst.exchange,
                       inst.strike_price,
                       inst.expiry_date);
    });

    refdata_sub_->set_on_perp([this](const refdata::PerpInstrument& inst) {
        perp_map_[inst.instrument_id] = {inst.underlying, inst.exchange_id};
        ygg::log::info("[Pricer] Perp instrument registered: id={} {} {}",
                       inst.instrument_id,
                       inst.underlying,
                       inst.exchange);
    });

    refdata_sub_->set_on_remove([this](uint64_t instrument_id) { builder_.remove_instrument(instrument_id); });

    ygg::log::info("[Pricer] Ready — entering main loop");
}

void PricerApp::run() {

    constexpr auto idle_sleep = std::chrono::microseconds(100);
    const auto publish_interval = std::chrono::milliseconds(settings_.publish_interval_ms);
    constexpr auto heartbeat_interval = std::chrono::seconds(5);
    constexpr auto ready_interval = std::chrono::seconds(30);

    auto last_publish = std::chrono::steady_clock::now();
    auto last_heartbeat = std::chrono::steady_clock::now();
    auto last_ready = std::chrono::steady_clock::now();
    uint64_t heartbeat_seq = 0;
    bool initial_ready_sent = false;

    while (ygg::signal::is_running()) {
        int fragments = 0;
        fragments += md_sub_->poll(64);
        fragments += refdata_sub_->poll(64);

        auto now = std::chrono::steady_clock::now();

        if (now - last_publish >= publish_interval) {
            const uint32_t today = today_yyyymmdd();
            auto grids = builder_.build(today);

            for (const auto& grid : grids) {
                vol_pub_->publish(grid, now_ns());
                ygg::log::debug("[Pricer] Published surface: {} {} points={}",
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
                status_pub_->publish_ready(now_ns(),
                                           exchanges_loaded,
                                           static_cast<uint16_t>(grids.size()),
                                           total_points);
                initial_ready_sent = true;
            }

            last_publish = now;
        }

        if (now - last_heartbeat >= heartbeat_interval) {
            status_pub_->publish_heartbeat(now_ns(), ++heartbeat_seq);
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
            status_pub_->publish_ready(now_ns(), exchanges_loaded, static_cast<uint16_t>(grids.size()), total_points);
            last_ready = now;
        }

        if (fragments == 0)
            std::this_thread::sleep_for(idle_sleep);
    }

    ygg::log::info("[Pricer] Shutdown complete.");
}

}  // namespace bpt::pricer
