#pragma once

/// \file
/// \brief Lifecycle service for bpt-radar.
///
/// Owns:
///   - The latest VolSurfaceGrid per (exchange_id, underlying) tuple.
///   - A rolling map of instrument_id → latest OI from InstrumentStats.
///
/// Run loop polls both subscriptions; every `publish_interval_ms` it walks
/// every cached surface, joins each strike's `instrument_id` against the OI
/// map, computes the per-(exchange, underlying) MarketColor frame, and
/// publishes one frame per tuple.

#include "radar/analysis/surface_point.h"
#include "radar/config/settings.h"
#include "radar/messaging/aeron_bus.h"

#include <messages/ExchangeId.h>
#include <messages/FundingRate.h>
#include <messages/InstrumentStats.h>
#include <messages/MdMarketData.h>
#include <messages/MdTrade.h>
#include <messages/VolSurface.h>

#include <bpt_app/app.h>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace bpt::radar {

class RadarService : public bpt::app::IService {
public:
    RadarService(config::Settings settings, messaging::RadarBus bus);

    void run() override;

private:
    /// \brief Snapshot of a surface keyed by (exchange, underlying).
    ///
    /// Held by value so the run loop can recompute color without re-decoding
    /// the SBE buffer. Refreshed every time on_vol_surface fires.
    struct SurfaceKey {
        uint8_t exchange_id;
        std::string underlying;

        bool operator==(const SurfaceKey& o) const noexcept {
            return exchange_id == o.exchange_id && underlying == o.underlying;
        }
    };
    struct SurfaceKeyHash {
        std::size_t operator()(const SurfaceKey& k) const noexcept {
            // FNV-ish blend; not load-bearing for perf, only used at publish cadence.
            std::size_t h = std::hash<std::string>{}(k.underlying);
            h ^= static_cast<std::size_t>(k.exchange_id) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            return h;
        }
    };

    void on_vol_surface(bpt::messages::VolSurface& surface);
    void on_instrument_stats(bpt::messages::InstrumentStats& stats);
    void on_funding(bpt::messages::FundingRate& fr);
    void on_refdata_perp(uint64_t instrument_id, const std::string& underlying, uint8_t exchange_id);
    void on_trade(bpt::messages::MdTrade& trade);
    void on_bbo(bpt::messages::MdMarketData& bbo);

    void publish_all();
    void publish_for(const SurfaceKey& key, std::vector<analysis::SurfacePoint>& cached);

    config::Settings settings_;
    messaging::RadarBus bus_;

    /// Latest decoded surface per (exchange, underlying).
    std::unordered_map<SurfaceKey, std::vector<analysis::SurfacePoint>, SurfaceKeyHash> surfaces_;

    /// Latest OI per instrument_id. Stays sticky between updates so a stale
    /// strike (gone from the surface for a tick) doesn't immediately drop
    /// OI to NaN.
    std::unordered_map<uint64_t, double> oi_by_instrument_;

    /// Funding rate state per perp instrument_id. Populated by FundingRate
    /// stream; joined to MarketColor entries via perp_id_by_key_ below.
    struct FundingState {
        double rate_8h{0.0};        ///< decimal (rate_bps from wire ÷ 1e6)
        uint64_t next_funding_ts_ns{0};
    };
    std::unordered_map<uint64_t, FundingState> funding_by_instrument_;

    /// Mark + index quotes per instrument_id, from InstrumentStats. Kept for
    /// every instrument we see (cheap — one row per perp/spot per venue), the
    /// lookup at publish time only retrieves perp rows via perp_id_by_key_.
    /// last_seen_ns lets publish_for() drop stale quotes to NaN basis.
    struct PerpQuote {
        double mark{0.0};
        double index{0.0};
        uint64_t last_seen_ns{0};
    };
    std::unordered_map<uint64_t, PerpQuote> perp_quote_by_instrument_;

    /// (exchange_id, underlying) → perp instrument_id. Populated by refdata
    /// snapshot; consulted during publish_for() to attach funding rate to
    /// the right MarketColor entry.
    std::unordered_map<SurfaceKey, uint64_t, SurfaceKeyHash> perp_id_by_key_;

    /// Reverse lookup: perp instrument_id → SurfaceKey. Kept in sync with
    /// perp_id_by_key_ so MdTrade's instrument_id can find the right flow
    /// bucket without scanning the perp_id_by_key_ map per trade.
    std::unordered_map<uint64_t, SurfaceKey> perp_key_by_id_;

    /// One MdTrade fragment for a perp we know about. Kept short-lived in the
    /// flow window; pruned on insert and on publish.
    struct TradeEntry {
        uint64_t ts_ns;
        bool buy;       ///< true = aggressor lifted offer (BUY); false = hit bid (SELL)
        double notional;  ///< price × qty
    };

    /// Rolling per-(exchange, underlying) flow window. Fixed 5min lookback —
    /// short enough to react to flow shifts, long enough that majors hit
    /// statistical mass on the perp.
    std::unordered_map<SurfaceKey, std::deque<TradeEntry>, SurfaceKeyHash> flow_window_by_key_;

    /// One BBO mid sample, throttled to ~5s spacing per perp. 1h window of
    /// these feeds the realized-vol calculation in publish_for().
    struct MidSample {
        uint64_t ts_ns;
        double mid;
    };
    std::unordered_map<SurfaceKey, std::deque<MidSample>, SurfaceKeyHash> mid_window_by_key_;

    uint64_t last_publish_ns_{0};
};

}  // namespace bpt::radar
