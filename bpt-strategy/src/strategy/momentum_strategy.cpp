#include "strategy/strategy/momentum_strategy.h"

#include "strategy/md/subscribe_helpers.h"
#include "strategy/refdata/exchange_id.h"

#include <messages/ExchangeId.h>
#include <messages/InstrumentType.h>
#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <messages/TimeInForce.h>

#include <bpt_common/logging.h>
#include <cmath>
#include <string>
#include <vector>

using bpt::messages::ExchangeId;
using bpt::messages::OrderSide;
using bpt::messages::OrderType;
using bpt::messages::TimeInForce;

namespace bpt::strategy::strategy {

namespace {
// Sub-module logger — auto-prefixed with "MomentumStrategy" via %(logger).
// Lazy-init because bpt::common::logging::init() runs after static init.
quill::Logger* kLog() {
    static quill::Logger* l = bpt::common::logging::get_logger("MomentumStrategy");
    return l;
}
}  // namespace

MomentumStrategy::MomentumStrategy(uint64_t correlation_id,
                                   const config::StrategyConfig& cfg,
                                   refdata::IRefdataClient& refdata,
                                   md::IMdClient* md,
                                   order::OrderManager* order_mgr)
    : correlation_id_(correlation_id),
      lookback_(static_cast<std::size_t>(cfg.params["lookback"].value<int64_t>().value_or(20))),
      entry_threshold_(cfg.params["entry_threshold"].value<double>().value_or(0.001)),
      cooldown_ns_(static_cast<uint64_t>(cfg.params["cooldown_ms"].value<int64_t>().value_or(5000)) * 1'000'000ULL),
      instruments_(cfg.instruments),
      md_exchanges_(cfg.md_exchanges),
      venue_exec_(cfg.venue_exec),
      refdata_(refdata),
      md_client_(md),
      order_mgr_(order_mgr) {
    bpt::common::log::info(kLog(),
                           "lookback={} entry_threshold={:.4f} cooldown_ms={}",
                           lookback_,
                           entry_threshold_,
                           cooldown_ns_ / 1'000'000ULL);
    bpt::common::log::info(kLog(),
                           "risk: max_position_usd={} max_order_size_usd={} max_open_orders={}",
                           cfg.risk.max_position_usd,
                           cfg.risk.max_order_size_usd,
                           cfg.risk.max_open_orders);
    bpt::common::log::info(kLog(),
                           "schedule: require_refdata_ready={} md_staleness_threshold_ms={}",
                           cfg.schedule.require_refdata_ready,
                           cfg.schedule.md_staleness_threshold_ms);

    if (instruments_.empty()) {
        bpt::common::log::info(kLog(), "instruments: ALL (no canonical filter)");
    } else {
        for (const auto& s : instruments_)
            bpt::common::log::info(kLog(), "instrument: {}", s);
    }
}

void MomentumStrategy::start() {
    if (md_exchanges_.empty()) {
        bpt::common::log::info(kLog(), "MD exchanges: ALL");
    } else {
        for (const auto& ex : md_exchanges_)
            bpt::common::log::info(kLog(), "MD exchange: {}", ex);
    }

    refdata_.subscribe(correlation_id_, CanonicalResolver::build_filters(instruments_, md_exchanges_));
}

void MomentumStrategy::on_snapshot(const refdata::InstrumentCache& cache) {
    bpt::common::log::info(kLog(), "Snapshot received ({} total instruments), resolving universe...", cache.size());

    state_.clear();

    for (const auto& r : CanonicalResolver::resolve_instruments(cache, instruments_, md_exchanges_)) {
        state_.emplace(r.instrument_id,
                       InstrumentState{.symbol = r.instrument.symbol,
                                       .exchange = r.instrument.exchange,
                                       .exchange_id = r.exchange_id});
        bpt::common::log::info("  [{}] {} @ {}", r.instrument_id, r.instrument.symbol, r.instrument.exchange);
    }

    bpt::common::log::info(kLog(), "Trading universe: {} instrument(s)", state_.size());

    if (!md_client_)
        return;
    auto subs = md::build_subscriptions(state_);
    bpt::common::log::info(kLog(), "Subscribing MD service to {} instrument(s)", subs.size());
    md_client_->subscribe(correlation_id_, subs);
}

void MomentumStrategy::on_delta(const refdata::Instrument& inst, bpt::messages::DeltaUpdateType::Value update_type) {
    if (update_type == bpt::messages::DeltaUpdateType::ADD) {
        if (!CanonicalResolver::matches(instruments_, md_exchanges_, inst))
            return;
        state_.emplace(inst.instrument_id, InstrumentState{.symbol = inst.symbol, .exchange = inst.exchange});
        bpt::common::log::info(kLog(), "Delta ADD {} @ {}", inst.symbol, inst.exchange);

    } else if (update_type == bpt::messages::DeltaUpdateType::REMOVE) {
        state_.erase(inst.instrument_id);
        bpt::common::log::info(kLog(), "Delta REMOVE {} @ {}", inst.symbol, inst.exchange);
    }
    // MODIFY: refdata fields (lot size, tick size) don't affect momentum state
}

void MomentumStrategy::on_bbo(const bpt::messages::MdMarketData& tick) {
    auto it = state_.find(tick.instrumentId());
    if (it == state_.end())
        return;

    InstrumentState& st = it->second;

    const double mid = (tick.bidPrice() + tick.askPrice()) * 0.5;

    st.prices.push_back(mid);
    if (st.prices.size() > lookback_)
        st.prices.pop_front();

    if (st.prices.size() < lookback_)
        return;  // window not yet full

    const double oldest = st.prices.front();
    if (oldest <= 0.0)
        return;

    const double momentum = (mid - oldest) / oldest;

    if (std::abs(momentum) >= entry_threshold_)
        emit_signal(tick.instrumentId(), st, mid, momentum, tick.timestampNs());
}

void MomentumStrategy::on_trade(const bpt::messages::MdTrade& /*tick*/) {
    // Momentum is driven by BBO mid-price, not individual trades.
}

void MomentumStrategy::emit_signal(uint64_t instrument_id,
                                   InstrumentState& state,
                                   double current_mid,
                                   double momentum,
                                   uint64_t timestamp_ns) {
    if (timestamp_ns < state.last_signal_ns + cooldown_ns_)
        return;

    state.last_signal_ns = timestamp_ns;

    const auto side = (momentum > 0.0) ? OrderSide::BUY : OrderSide::SELL;

    // Look up venue execution config for this exchange.
    const auto it = venue_exec_.find(state.exchange);
    if (it == venue_exec_.end() || !it->second.enabled) {
        bpt::common::log::debug(kLog(), "Venue {} not enabled — signal suppressed", state.exchange);
        return;
    }
    const auto& vex = it->second;

    const auto order_type = (vex.order_type == "MARKET") ? OrderType::MARKET : OrderType::LIMIT;
    const auto tif = (vex.tif == "IOC") ? TimeInForce::IOC : (vex.tif == "FOK") ? TimeInForce::FOK : TimeInForce::GTC;

    if (!order_mgr_) {
        bpt::common::log::info(kLog(),
                               "SIGNAL {} {} @ {} mid={:.6f} momentum={:+.4f}% (no gateway)",
                               (side == OrderSide::BUY ? "BUY" : "SELL"),
                               state.symbol,
                               state.exchange,
                               current_mid,
                               momentum * 100.0);
        return;
    }

    // Price: natural units. MARKET orders pass 0; LIMIT uses current mid with a small edge.
    const double price_f = (order_type == OrderType::MARKET) ? 0.0
                           : (side == OrderSide::BUY)        ? current_mid * 1.001
                                                             : current_mid * 0.999;

    // Quantity: 0.001 base currency in natural units (small size for testnet).
    static constexpr double kQty = 0.001;

    bpt::common::log::info(kLog(),
                           "SIGNAL {} {} @ {} mid={:.6f} momentum={:+.4f}%",
                           (side == OrderSide::BUY ? "BUY" : "SELL"),
                           state.symbol,
                           state.exchange,
                           current_mid,
                           momentum * 100.0);

    const uint64_t order_id =
        order_mgr_->place_order(instrument_id, state.exchange_id, side, order_type, tif, price_f, kQty);
    if (order_id != 0)
        bpt::common::log::info(kLog(), "order placed → order_id={}", order_id);
}

}  // namespace bpt::strategy::strategy
