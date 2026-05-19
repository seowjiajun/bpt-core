#pragma once

#include "pricer/config/settings.h"
#include "pricer/md/api/md_subscribe_client.h"
#include "pricer/messaging/aeron_bus.h"
#include "pricer/surface/surface_builder.h"

#include <messages/ExchangeId.h>

#include <bpt_app/app.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bpt::pricer {

class PricerService : public bpt::app::IService {
public:
    PricerService(config::Settings settings, messaging::PricerBus bus);
    void run() override;

    /// \name Handler interface — called by the templated subscribers in the bus.
    /// @{
    void on_bbo(uint64_t instrument_id, double bid, double ask, uint64_t timestamp_ns);
    void on_trade(uint64_t /*instrument_id*/, double /*price*/, double /*qty*/, uint64_t /*timestamp_ns*/) noexcept {}
    void on_refdata_option(const surface::OptionInstrument& inst);
    void on_refdata_perp(const refdata::PerpInstrument& inst);
    void on_refdata_remove(uint64_t instrument_id);
    /// @}

private:
    struct PerpInfo {
        std::string underlying;
        bpt::messages::ExchangeId::Value exchange_id;
    };

    /// \brief Pending option universe — populated as refdata snapshots arrive,
    ///        consumed by the periodic re-subscription pass.
    ///
    /// We collect options off the on_option callback (which fires for every
    /// option in every refdata snapshot replay) and re-apply the filter
    /// periodically rather than per-option. That batches the venue-side
    /// subscribe traffic and keeps the universe consistent across the
    /// snapshot boundary.
    struct OptionDesc {
        uint64_t instrument_id;
        std::string underlying;
        std::string exchange;
        std::string venue_symbol;
        uint32_t expiry_date;
        double strike_price;
        bool is_call;
    };

    void maybe_resubscribe_options();

    /// \brief Build the filtered subscribe batch from `option_universe_`.
    ///
    /// Filter: keep options for each underlying whose expiry is one of the
    /// front-N (earliest non-expired) expiries. Optional per-expiry strike
    /// cap if `settings_.universe.max_strikes_per_expiry > 0`.
    std::vector<md::api::MdSubscribeClient::InstrumentDesc> build_subscribe_batch() const;

    config::Settings settings_;
    surface::SurfaceBuilder builder_;
    messaging::PricerBus bus_;
    std::unordered_map<uint64_t, PerpInfo> perp_map_;

    /// instrument_id → full option metadata. Rebuilt every snapshot
    /// (snapshots replay the full universe, not just deltas).
    std::unordered_map<uint64_t, OptionDesc> option_universe_;

    /// Set when option_universe_ changes; cleared after re-subscribe.
    bool universe_dirty_{false};

    /// Stable correlation_id stamped on every MdSubscribeBatch pricer sends.
    /// Md-gateway uses this to scope this consumer's desired set in its
    /// per-consumer refcounting, so pricer doesn't fight strategy.
    uint64_t md_correlation_id_{0};
};

}  // namespace bpt::pricer
