#include "pricer/app/pricer_service.h"

#include <algorithm>
#include <bpt_common/logging.h>
#include <bpt_common/signal.h>
#include <bpt_common/util/tsc_clock.h>
#include <chrono>
#include <ctime>
#include <set>
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
      bus_(std::move(bus)),
      md_correlation_id_(bpt::common::util::TscClock::now_epoch_ns()) {
    bus_.md_sub->set_bbo_callback([this](uint64_t instrument_id, double bid, double ask, uint64_t timestamp_ns) {
        auto pit = perp_map_.find(instrument_id);
        if (pit != perp_map_.end()) {
            builder_.set_spot(pit->second.underlying, pit->second.exchange_id, (bid + ask) * 0.5);
            return;
        }
        builder_.on_bbo(instrument_id, bid, ask, timestamp_ns);
    });

    bus_.refdata_sub->set_on_option([this](const surface::OptionInstrument& inst) { on_refdata_option(inst); });

    bus_.refdata_sub->set_on_perp([this](const refdata::PerpInstrument& inst) {
        perp_map_[inst.instrument_id] = {inst.underlying, inst.exchange_id};
        // Treat a perp as a (degenerate) option entry with a sentinel expiry
        // of 0 so it's always included in the subscribe batch regardless of
        // front-N filter. The batch needs perps so we can compute forwards.
        option_universe_[inst.instrument_id] = OptionDesc{
            .instrument_id = inst.instrument_id,
            .underlying = inst.underlying,
            .exchange = inst.exchange,
            .venue_symbol = inst.venue_symbol,
            .expiry_date = 0,
            .strike_price = 0.0,
            .is_call = false,
        };
        universe_dirty_ = true;
        bpt::common::log::info("Perp instrument registered: id={} {} {} symbol={}",
                               inst.instrument_id,
                               inst.underlying,
                               inst.exchange,
                               inst.venue_symbol);
    });

    bus_.refdata_sub->set_on_remove([this](uint64_t instrument_id) {
        builder_.remove_instrument(instrument_id);
        if (option_universe_.erase(instrument_id) > 0)
            universe_dirty_ = true;
    });

    bpt::common::log::info("publish_interval_ms={} risk_free_rate={:.4f} md_correlation_id={}",
                           settings_.publish_interval_ms,
                           settings_.risk_free_rate,
                           md_correlation_id_);
    for (const auto& u : settings_.underlyings)
        bpt::common::log::info("underlying: {}", u);
    for (const auto& e : settings_.exchanges)
        bpt::common::log::info("exchange: {}", e);
    bpt::common::log::info("Ready — entering main loop");
}

void PricerService::on_refdata_option(const surface::OptionInstrument& inst) {
    builder_.add_instrument(inst);
    option_universe_[inst.instrument_id] = OptionDesc{
        .instrument_id = inst.instrument_id,
        .underlying = inst.underlying,
        .exchange = inst.exchange,
        .venue_symbol = inst.venue_symbol,
        .expiry_date = inst.expiry_date,
        .strike_price = inst.strike_price,
        .is_call = inst.is_call,
    };
    universe_dirty_ = true;
    // Per-option log was a few-hundred-MB problem at 2k instruments per
    // snapshot republish; demote to debug.
    bpt::common::log::debug("Option instrument: id={} {} {} K={} exp={} symbol={}",
                            inst.instrument_id,
                            inst.underlying,
                            inst.exchange,
                            inst.strike_price,
                            inst.expiry_date,
                            inst.venue_symbol);
}

std::vector<md::MdSubscribeClient::InstrumentDesc> PricerService::build_subscribe_batch() const {
    // First pass: collect distinct expiries per underlying. expiry_date == 0
    // is the perp sentinel — always include those.
    std::unordered_map<std::string, std::set<uint32_t>> expiries_by_underlying;
    for (const auto& [_, opt] : option_universe_) {
        if (opt.expiry_date == 0)
            continue;
        expiries_by_underlying[opt.underlying].insert(opt.expiry_date);
    }

    // Second pass: pick the front-N expiries per underlying (sorted ascending,
    // first N). std::set is already sorted.
    std::unordered_map<std::string, std::set<uint32_t>> kept_expiries;
    const uint32_t front_n = settings_.universe.front_n_expiries;
    for (const auto& [underlying, expiries] : expiries_by_underlying) {
        std::set<uint32_t>& keep = kept_expiries[underlying];
        uint32_t taken = 0;
        for (uint32_t e : expiries) {
            if (front_n > 0 && taken >= front_n)
                break;
            keep.insert(e);
            ++taken;
        }
    }

    // Third pass: include perps (always) + options whose expiry is in the kept
    // set for their underlying. Apply per-expiry strike cap if configured.
    // Bucket key is "underlying|expiry"; std::string is simpler than a custom
    // hasher for std::pair, and the cost is irrelevant at filter cadence.
    std::unordered_map<std::string, std::vector<OptionDesc>> bucketed;
    auto bucket_key = [](const std::string& u, uint32_t e) { return u + "|" + std::to_string(e); };

    std::vector<md::MdSubscribeClient::InstrumentDesc> batch;
    batch.reserve(option_universe_.size());

    for (const auto& [_, opt] : option_universe_) {
        if (opt.expiry_date == 0) {
            batch.push_back(md::MdSubscribeClient::InstrumentDesc{opt.instrument_id, opt.exchange, opt.venue_symbol, 0});
            continue;
        }
        auto kit = kept_expiries.find(opt.underlying);
        if (kit == kept_expiries.end())
            continue;
        if (kit->second.find(opt.expiry_date) == kit->second.end())
            continue;
        bucketed[bucket_key(opt.underlying, opt.expiry_date)].push_back(opt);
    }

    const uint32_t max_per_expiry = settings_.universe.max_strikes_per_expiry;
    for (auto& [_, opts] : bucketed) {
        // Optional cap: keep strikes closest to the median (stand-in for ATM
        // until forward-aware filtering lands).
        if (max_per_expiry > 0 && opts.size() > max_per_expiry) {
            std::sort(opts.begin(), opts.end(),
                      [](const OptionDesc& a, const OptionDesc& b) { return a.strike_price < b.strike_price; });
            const std::size_t median_idx = opts.size() / 2;
            const std::size_t half = max_per_expiry / 2;
            const std::size_t lo = (median_idx > half) ? median_idx - half : 0;
            const std::size_t hi = std::min(opts.size(), lo + max_per_expiry);
            opts.erase(opts.begin() + static_cast<std::ptrdiff_t>(hi), opts.end());
            opts.erase(opts.begin(), opts.begin() + static_cast<std::ptrdiff_t>(lo));
        }
        for (const auto& opt : opts)
            batch.push_back(md::MdSubscribeClient::InstrumentDesc{opt.instrument_id, opt.exchange, opt.venue_symbol, 0});
    }

    return batch;
}

void PricerService::maybe_resubscribe_options() {
    if (!universe_dirty_)
        return;
    auto batch = build_subscribe_batch();
    if (batch.empty()) {
        // Nothing to subscribe yet — refdata may not have published any
        // options. Leave dirty=true so the next call retries when more
        // instruments land.
        return;
    }
    bus_.md_ctrl->publish(md_correlation_id_, batch);
    bpt::common::log::info("[PricerService] re-subscribed md-gateway: {} instruments (from universe of {})",
                           batch.size(),
                           option_universe_.size());
    universe_dirty_ = false;
}

void PricerService::run() {
    constexpr auto idle_sleep = std::chrono::microseconds(100);
    const auto publish_interval = std::chrono::milliseconds(settings_.publish_interval_ms);
    constexpr auto heartbeat_interval = std::chrono::seconds(5);
    constexpr auto ready_interval = std::chrono::seconds(30);
    constexpr auto resubscribe_interval = std::chrono::seconds(2);  // re-check filter on this cadence

    auto last_publish = std::chrono::steady_clock::now();
    auto last_heartbeat = std::chrono::steady_clock::now();
    auto last_ready = std::chrono::steady_clock::now();
    auto last_resubscribe = std::chrono::steady_clock::now() - resubscribe_interval;
    uint64_t heartbeat_seq = 0;
    bool initial_ready_sent = false;

    while (bpt::common::signal::is_running()) {
        int fragments = 0;
        fragments += bus_.md_sub->poll(64);
        fragments += bus_.refdata_sub->poll(64);

        auto now = std::chrono::steady_clock::now();

        if (now - last_resubscribe >= resubscribe_interval) {
            maybe_resubscribe_options();
            last_resubscribe = now;
        }

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
