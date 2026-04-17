#include "strategy/strategy/momentum_strategy.h"

#include <messages/ExchangeId.h>
#include <messages/InstrumentType.h>
#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <messages/TimeInForce.h>

#include <cmath>
#include <string>
#include <vector>

using bpt::messages::ExchangeId;
using bpt::messages::OrderSide;
using bpt::messages::OrderType;
using bpt::messages::TimeInForce;

namespace bpt::strategy::strategy {

MomentumStrategy::MomentumStrategy(uint64_t correlation_id,
                                   const config::StrategyConfig& cfg,
                                   refdata::RefdataClient& refdata,
                                   md::MdClient* md,
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
    ygg::log::info("[MomentumStrategy] lookback={} entry_threshold={:.4f} cooldown_ms={}",
                   lookback_,
                   entry_threshold_,
                   cooldown_ns_ / 1'000'000ULL);
    ygg::log::info(
        "[MomentumStrategy] risk: max_position_usd={} max_order_size_usd={} "
        "max_daily_loss_usd={} max_open_orders={}",
        cfg.risk.max_position_usd,
        cfg.risk.max_order_size_usd,
        cfg.risk.max_daily_loss_usd,
        cfg.risk.max_open_orders);
    ygg::log::info("[MomentumStrategy] schedule: require_refdata_ready={} md_staleness_threshold_ms={}",
                   cfg.schedule.require_refdata_ready,
                   cfg.schedule.md_staleness_threshold_ms);

    if (instruments_.empty()) {
        ygg::log::info("[MomentumStrategy] instruments: ALL (no canonical filter)");
    } else {
        for (const auto& s : instruments_)
            ygg::log::info("[MomentumStrategy] instrument: {}", s);
    }
}

void MomentumStrategy::start() {
    if (md_exchanges_.empty()) {
        ygg::log::info("[MomentumStrategy] MD exchanges: ALL");
    } else {
        for (const auto& ex : md_exchanges_)
            ygg::log::info("[MomentumStrategy] MD exchange: {}", ex);
    }

    std::vector<refdata::RefdataClient::CanonicalFilter> filters;
    for (const auto& sym : instruments_) {
        if (auto parsed = CanonicalResolver::parse(sym)) {
            const auto sbe_type = [&]() {
                using T = refdata::InstrumentType;
                using S = bpt::messages::InstrumentType;
                switch (parsed->type) {
                    case T::SPOT:
                        return S::SPOT;
                    case T::PERPETUAL:
                        return S::PERPETUAL;
                    case T::FUTURE:
                        return S::FUTURE;
                    case T::OPTION:
                        return S::OPTION;
                    default:
                        return S::NULL_VALUE;
                }
            }();
            if (md_exchanges_.empty()) {
                filters.push_back({parsed->base, parsed->quote, sbe_type, ""});
            } else {
                for (const auto& ex : md_exchanges_)
                    filters.push_back({parsed->base, parsed->quote, sbe_type, ex});
            }
        }
    }
    refdata_.subscribe(correlation_id_, std::move(filters));
}

void MomentumStrategy::on_snapshot(const refdata::InstrumentCache& cache) {
    ygg::log::info("[MomentumStrategy] Snapshot received ({} total instruments), resolving universe...", cache.size());

    state_.clear();

    const auto ids = CanonicalResolver::resolve(cache, instruments_, md_exchanges_);

    for (uint64_t id : ids) {
        const auto inst = cache.get(id);
        if (!inst)
            continue;
        auto ex_id = ExchangeId::NULL_VALUE;
        if (inst->exchange == "BINANCE")
            ex_id = ExchangeId::BINANCE;
        else if (inst->exchange == "OKX")
            ex_id = ExchangeId::OKX;
        else if (inst->exchange == "HYPERLIQUID")
            ex_id = ExchangeId::HYPERLIQUID;
        state_.emplace(id, InstrumentState{.symbol = inst->symbol, .exchange = inst->exchange, .exchange_id = ex_id});
        ygg::log::info("  [{}] {} @ {}", id, inst->symbol, inst->exchange);
    }

    ygg::log::info("[MomentumStrategy] Trading universe: {} instrument(s)", state_.size());

    if (!md_client_)
        return;

    std::vector<md::MdClient::InstrumentDesc> subs;
    subs.reserve(state_.size());
    for (const auto& [id, st] : state_)
        subs.push_back({id, st.exchange, st.symbol});

    ygg::log::info("[MomentumStrategy] Subscribing MD service to {} instrument(s)", subs.size());
    md_client_->subscribe(correlation_id_, subs);
}

void MomentumStrategy::on_delta(const refdata::Instrument& inst,
                                bpt::messages::DeltaUpdateType::Value update_type) {
    if (update_type == bpt::messages::DeltaUpdateType::ADD) {
        // Re-run resolver for the single new instrument against our universe.
        const auto ids = CanonicalResolver::resolve(refdata_.cache(), instruments_, md_exchanges_);
        const bool wanted = std::find(ids.begin(), ids.end(), inst.instrument_id) != ids.end();
        if (!wanted)
            return;
        state_.emplace(inst.instrument_id, InstrumentState{.symbol = inst.symbol, .exchange = inst.exchange});
        ygg::log::info("[MomentumStrategy] Delta ADD {} @ {}", inst.symbol, inst.exchange);

    } else if (update_type == bpt::messages::DeltaUpdateType::REMOVE) {
        state_.erase(inst.instrument_id);
        ygg::log::info("[MomentumStrategy] Delta REMOVE {} @ {}", inst.symbol, inst.exchange);
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
        ygg::log::debug("[MomentumStrategy] Venue {} not enabled — signal suppressed", state.exchange);
        return;
    }
    const auto& vex = it->second;

    const auto order_type = (vex.order_type == "MARKET") ? OrderType::MARKET : OrderType::LIMIT;
    const auto tif = (vex.tif == "IOC") ? TimeInForce::IOC : (vex.tif == "FOK") ? TimeInForce::FOK : TimeInForce::GTC;

    if (!order_mgr_) {
        ygg::log::info("[MomentumStrategy] SIGNAL {} {} @ {} mid={:.6f} momentum={:+.4f}% (no gateway)",
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

    ygg::log::info("[MomentumStrategy] SIGNAL {} {} @ {} mid={:.6f} momentum={:+.4f}%",
                   (side == OrderSide::BUY ? "BUY" : "SELL"),
                   state.symbol,
                   state.exchange,
                   current_mid,
                   momentum * 100.0);

    const uint64_t order_id =
        order_mgr_->place_order(instrument_id, state.exchange_id, side, order_type, tif, price_f, kQty);
    if (order_id != 0)
        ygg::log::info("[MomentumStrategy] order placed → order_id={}", order_id);
}

}  // namespace bpt::strategy::strategy
