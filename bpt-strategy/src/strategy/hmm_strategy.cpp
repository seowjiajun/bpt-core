#include "strategy/strategy/hmm_strategy.h"

#include <messages/DeltaUpdateType.h>
#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/InstrumentType.h>
#include <messages/RejectReason.h>
#include <messages/RejectSource.h>

#include <algorithm>
#include <cmath>

using bpt::messages::ExchangeId;
using bpt::messages::ExecStatus;
using bpt::messages::OrderSide;
using bpt::messages::OrderType;
using bpt::messages::RejectSource;
using bpt::messages::TimeInForce;

namespace bpt::strategy::strategy {

static constexpr double kQtyScale = 1e8;
static constexpr double kPriceScale = 1e8;
static constexpr uint64_t kBarIntervalDefault = 1'000'000'000ULL;    // 1 s in ns
static constexpr uint64_t kTransitionTimeoutNs = 10'000'000'000ULL;  // 10 s
static constexpr size_t kMinWarmupBars = 30;

// ── Constructor ──────────────────────────────────────────────────────────────

HmmStrategy::HmmStrategy(uint64_t correlation_id,
                         const config::StrategyConfig& cfg,
                         refdata::RefdataClient& refdata,
                         md::MdClient* md,
                         order::OrderManager* order_mgr)
    : confidence_threshold_(cfg.params["hmm_confidence"].value<double>().value_or(0.50)),
      min_dwell_bars_(static_cast<int>(cfg.params["hmm_min_dwell_bars"].value<int64_t>().value_or(5))),
      ewma_lambda_(cfg.params["ewma_lambda"].value<double>().value_or(0.94)),
      bar_interval_ns_(cfg.params["bar_interval_s"].value<double>()
                           ? static_cast<uint64_t>(*cfg.params["bar_interval_s"].value<double>() * 1e9)
                           : kBarIntervalDefault),
      order_book_depth_(static_cast<uint8_t>(cfg.params["order_book_depth"].value<int64_t>().value_or(5))),
      ema_fast_period_(static_cast<int>(cfg.params["ema_fast_period"].value<int64_t>().value_or(8))),
      ema_slow_period_(static_cast<int>(cfg.params["ema_slow_period"].value<int64_t>().value_or(21))),
      atr_period_(static_cast<int>(cfg.params["atr_period"].value<int64_t>().value_or(14))),
      atr_stop_mult_(cfg.params["atr_stop_mult"].value<double>().value_or(1.5)),
      atr_target_mult_(cfg.params["atr_target_mult"].value<double>().value_or(2.5)),
      momentum_qty_usd_(cfg.params["momentum_qty_usd"].value<double>().value_or(500.0)),
      vwap_deviation_atr_(cfg.params["vwap_deviation_atr"].value<double>().value_or(1.2)),
      vwap_close_atr_(cfg.params["vwap_close_atr"].value<double>().value_or(0.3)),
      vwap_stop_atr_(cfg.params["vwap_stop_atr"].value<double>().value_or(2.0)),
      reversion_qty_usd_(cfg.params["reversion_qty_usd"].value<double>().value_or(500.0)),
      mm_gamma_(cfg.params["mm_gamma"].value<double>().value_or(0.1)),
      mm_k_(cfg.params["mm_k"].value<double>().value_or(1.5)),
      mm_horizon_s_(cfg.params["mm_horizon_s"].value<double>().value_or(1.0)),
      mm_qty_usd_(cfg.params["mm_qty_usd"].value<double>().value_or(200.0)),
      mm_max_inventory_usd_(cfg.params["mm_max_inventory_usd"].value<double>().value_or(1000.0)),
      mm_requote_ns_(static_cast<uint64_t>(cfg.params["mm_requote_ms"].value<double>().value_or(200.0) * 1e6)),
      mm_min_spread_bps_(cfg.params["mm_min_spread_bps"].value<double>().value_or(2.0)),
      aggress_bps_(cfg.params["aggress_bps"].value<double>().value_or(0.5)),
      max_spread_bps_(cfg.params["max_spread_bps"].value<double>().value_or(30.0)),
      correlation_id_(correlation_id),
      instruments_(cfg.instruments),
      md_exchanges_(cfg.md_exchanges),
      venue_exec_(cfg.venue_exec),
      refdata_(refdata),
      md_client_(md),
      order_mgr_(order_mgr) {
    ygg::log::info("[HMM] confidence={:.2f} min_dwell={} bar={:.1f}s depth={}",
                   confidence_threshold_,
                   min_dwell_bars_,
                   bar_interval_ns_ / 1e9,
                   order_book_depth_);
    ygg::log::info("[HMM] momentum: ema={}/{} atr={} stop={:.1f}x target={:.1f}x qty_usd={:.0f}",
                   ema_fast_period_,
                   ema_slow_period_,
                   atr_period_,
                   atr_stop_mult_,
                   atr_target_mult_,
                   momentum_qty_usd_);
    ygg::log::info("[HMM] reversion: dev={:.1f}x_atr close={:.1f}x_atr stop={:.1f}x_atr qty_usd={:.0f}",
                   vwap_deviation_atr_,
                   vwap_close_atr_,
                   vwap_stop_atr_,
                   reversion_qty_usd_);
    ygg::log::info(
        "[HMM] mm: gamma={:.2f} k={:.2f} horizon={:.1f}s qty_usd={:.0f} "
        "max_inv_usd={:.0f} requote_ms={} min_spread={:.1f}bps",
        mm_gamma_,
        mm_k_,
        mm_horizon_s_,
        mm_qty_usd_,
        mm_max_inventory_usd_,
        mm_requote_ns_ / 1'000'000,
        mm_min_spread_bps_);
}

// ── IStrategy lifecycle ──────────────────────────────────────────────────────

void HmmStrategy::start() {
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

void HmmStrategy::on_snapshot(const refdata::InstrumentCache& cache) {
    if (!state_.empty()) {
        ygg::log::debug("[HMM] Ignoring duplicate snapshot ({} instruments)", cache.size());
        return;
    }

    const auto all_ids = CanonicalResolver::resolve(cache, instruments_, md_exchanges_);
    for (uint64_t id : all_ids) {
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

        InstrumentState st;
        st.instrument_id = id;
        st.symbol = inst->symbol;
        st.exchange = inst->exchange;
        st.exchange_id = ex_id;
        st.tick_size = inst->tick_size;
        st.lot_size = inst->lot_size;

        ygg::log::info("[HMM] Instrument [{}] {} @ {} tick={} lot={}",
                       id,
                       inst->symbol,
                       inst->exchange,
                       inst->tick_size,
                       inst->lot_size);
        state_.emplace(id, std::move(st));
    }
    ygg::log::info("[HMM] Resolved {} instrument(s)", state_.size());

    if (!md_client_)
        return;
    std::vector<md::MdClient::InstrumentDesc> subs;
    for (const auto& [id, st] : state_)
        subs.push_back({id, st.exchange, st.symbol, order_book_depth_});
    md_client_->subscribe(correlation_id_, subs);
}

void HmmStrategy::on_delta(const refdata::Instrument& /*inst*/, bpt::messages::DeltaUpdateType::Value /*type*/) {}

// ── Market data ──────────────────────────────────────────────────────────────

void HmmStrategy::on_bbo(const bpt::messages::MdMarketData& tick) {
    auto it = state_.find(tick.instrumentId());
    if (it == state_.end())
        return;

    InstrumentState& st = it->second;
    const double bid = tick.bidPrice();
    const double ask = tick.askPrice();
    if (bid <= 0.0 || ask <= 0.0 || ask <= bid)
        return;

    st.bid = bid;
    st.ask = ask;
    st.last_bbo_ns = tick.timestampNs();
    const double mid = (bid + ask) * 0.5;

    // ── Build time bar ──
    if (st.bar_start_ns == 0) {
        st.bar_start_ns = st.last_bbo_ns;
        st.bar_open = st.bar_high = st.bar_low = st.bar_close = mid;
    } else {
        st.bar_close = mid;
        if (mid > st.bar_high)
            st.bar_high = mid;
        if (mid < st.bar_low)
            st.bar_low = mid;

        if (st.last_bbo_ns - st.bar_start_ns >= bar_interval_ns_) {
            on_bar_close(st);
            // Reset for new bar — accumulator fields cleared here after on_bar_close reads them.
            st.bar_start_ns = st.last_bbo_ns;
            st.bar_open = st.bar_high = st.bar_low = st.bar_close = mid;
            st.bar_trade_vol = 0.0;
            st.bar_vwap_num = 0.0;
            st.bar_vwap_den = 0.0;
        }
    }

    if (st.warming_up || st.transitioning) {
        // Transition timeout: force-complete after 10 s.
        if (st.transitioning && st.last_bbo_ns - st.transition_start_ns > kTransitionTimeoutNs) {
            ygg::log::warn("[HMM] {} transition timeout — force completing ({} pending cancels)",
                           st.symbol,
                           st.pending_cancels.size());
            for (uint64_t oid : st.pending_cancels)
                order_to_instrument_.erase(oid);
            st.pending_cancels.clear();
            if (st.close_order_id != 0) {
                order_to_instrument_.erase(st.close_order_id);
                st.close_order_id = 0;
            }
            st.closing_position = false;
            check_transition_complete(st);
        }
        return;
    }

    // ── Intra-bar: check active position exits ──
    const auto r = st.regime;
    if ((r == HmmFilter::State::TRENDING_UP || r == HmmFilter::State::TRENDING_DOWN) && st.has_momentum_position) {
        momentum_check_exit(st, mid);
    } else if (r == HmmFilter::State::MEAN_REVERT && st.has_reversion_position) {
        reversion_check_exit(st, mid);
    }
}

void HmmStrategy::on_trade(const bpt::messages::MdTrade& tick) {
    auto it = state_.find(tick.instrumentId());
    if (it == state_.end())
        return;

    InstrumentState& st = it->second;
    const double price = tick.price();
    const double qty = tick.qty();
    if (price <= 0.0 || qty <= 0.0)
        return;

    st.bar_trade_vol += qty;
    st.bar_vwap_num += price * qty;
    st.bar_vwap_den += qty;
}

void HmmStrategy::on_order_book(const bpt::messages::MdOrderBook& book) {
    auto it = state_.find(book.instrumentId());
    if (it == state_.end())
        return;
    InstrumentState& st = it->second;

    // Extract level-0 bid/ask quantities for the book-imbalance HMM feature.
    // SBE group iterators share the parent position pointer, so all bids must be
    // consumed before calling asks() — otherwise asks() reads from mid-bids-data.
    auto& mutable_book = const_cast<bpt::messages::MdOrderBook&>(book);
    auto& bids = mutable_book.bids();
    double bq = 0.0;
    if (bids.hasNext()) {
        bq = static_cast<double>(bids.next().qty()) / kQtyScale;
        while (bids.hasNext())
            bids.next();  // drain so position reaches asks header
    }
    auto& asks = mutable_book.asks();
    double aq = 0.0;
    if (asks.hasNext())
        aq = static_cast<double>(asks.next().qty()) / kQtyScale;

    if (bq + aq > 0.0)
        st.book_imbalance = bq / (bq + aq);
}

// ── Bar close ─────────────────────────────────────────────────────────────────

void HmmStrategy::on_bar_close(InstrumentState& st) {
    const double close = st.bar_close;

    // ── ATR: EMA of true range ──
    double tr = st.bar_high - st.bar_low;
    if (st.prev_bar_close > 0.0) {
        tr = std::max({tr, std::abs(st.bar_high - st.prev_bar_close), std::abs(st.bar_low - st.prev_bar_close)});
    }
    if (st.bar_atr_warmup == 0) {
        st.bar_atr = tr;
        st.bar_atr_warmup = 1;
    } else {
        const double alpha = 2.0 / (atr_period_ + 1.0);
        st.bar_atr = alpha * tr + (1.0 - alpha) * st.bar_atr;
        ++st.bar_atr_warmup;
    }

    if (st.prev_bar_close > 0.0) {
        const double lr = std::log(close / st.prev_bar_close);

        // ── Feature 0: rolling 1-min log return (ring buffer) ──
        if (st.return_count == InstrumentState::kReturnWindow)
            st.return_sum -= st.bar_returns[st.return_head];
        st.bar_returns[st.return_head] = lr;
        st.return_head = (st.return_head + 1) % InstrumentState::kReturnWindow;
        if (st.return_count < InstrumentState::kReturnWindow)
            ++st.return_count;
        st.return_sum += lr;

        // ── Feature 1: EWMA variance (RiskMetrics λ=0.94) ──
        st.ewma_var = ewma_lambda_ * st.ewma_var + (1.0 - ewma_lambda_) * lr * lr;
    }
    st.prev_bar_close = close;

    // ── Feature 4: trade-volume Z-score rolling window ──
    {
        const double v = st.bar_trade_vol;
        if (st.vol_count == InstrumentState::kVolWindow) {
            const double old = st.vol_buf[st.vol_head];
            st.vol_sum -= old;
            st.vol_sum_sq -= old * old;
        }
        st.vol_buf[st.vol_head] = v;
        st.vol_head = (st.vol_head + 1) % InstrumentState::kVolWindow;
        if (st.vol_count < InstrumentState::kVolWindow)
            ++st.vol_count;
        st.vol_sum += v;
        st.vol_sum_sq += v * v;
    }

    // ── Rolling VWAP window ──
    {
        if (st.vwap_count == InstrumentState::kVwapWindow) {
            st.vwap_num_sum -= st.vwap_num_buf[st.vwap_head];
            st.vwap_den_sum -= st.vwap_den_buf[st.vwap_head];
        }
        st.vwap_num_buf[st.vwap_head] = st.bar_vwap_num;
        st.vwap_den_buf[st.vwap_head] = st.bar_vwap_den;
        st.vwap_head = (st.vwap_head + 1) % InstrumentState::kVwapWindow;
        if (st.vwap_count < InstrumentState::kVwapWindow)
            ++st.vwap_count;
        st.vwap_num_sum += st.bar_vwap_num;
        st.vwap_den_sum += st.bar_vwap_den;
    }

    // ── EMA indicators (momentum sub-strategy) ──
    momentum_update_ema(st, close);

    if (st.return_count < kMinWarmupBars) {
        st.warming_up = true;
        return;
    }

    // ── HMM inference + regime selection ──
    evaluate_regime(st);

    if (st.transitioning)
        return;

    const double mid = (st.bid + st.ask) * 0.5;
    const double spread_bps = (st.ask - st.bid) / mid * 10000.0;
    const bool spread_ok = spread_bps <= max_spread_bps_;

    switch (st.regime) {
        case HmmFilter::State::TRENDING_UP:
        case HmmFilter::State::TRENDING_DOWN:
            if (spread_ok && !st.has_momentum_position)
                momentum_check_signal(st);
            else if (st.has_momentum_position)
                momentum_check_exit(st, mid);
            break;

        case HmmFilter::State::MEAN_REVERT:
            if (spread_ok && !st.has_reversion_position)
                reversion_check_signal(st);
            else if (st.has_reversion_position)
                reversion_check_exit(st, mid);
            break;

        case HmmFilter::State::HIGH_VOL:
            if (spread_ok)
                mm_quote(st);
            else
                mm_cancel_quotes(st);
            break;
    }

    ygg::log::debug(
        "[HMM] {} regime={} conf={:.2f} dwell={} mid={:.2f} "
        "atr={:.4f} ewma_vol={:.6f} spread={:.1f}bps",
        st.symbol,
        HmmFilter::state_name(st.regime),
        st.hmm.confidence(),
        st.regime_dwell,
        mid,
        st.bar_atr,
        std::sqrt(st.ewma_var),
        spread_bps);
}

// ── HMM inference ─────────────────────────────────────────────────────────────

void HmmStrategy::evaluate_regime(InstrumentState& st) {
    const double mid = (st.bid + st.ask) * 0.5;
    if (mid <= 0.0)
        return;

    // ── Build 5-feature observation vector ──
    std::array<double, HmmFilter::D> obs{};

    obs[0] = st.return_sum;                      // 1-min log return
    obs[1] = std::sqrt(st.ewma_var);             // per-bar realised vol (std dev)
    obs[2] = (st.ask - st.bid) / mid * 10000.0;  // spread bps
    obs[3] = st.book_imbalance;                  // top-of-book imbalance [0,1]

    // Volume z-score (default 0 if not enough history).
    if (st.vol_count >= 5) {
        const double mean = st.vol_sum / st.vol_count;
        const double var = st.vol_sum_sq / st.vol_count - mean * mean;
        const double stdv = var > 0.0 ? std::sqrt(var) : 1.0;
        obs[4] = (st.bar_trade_vol - mean) / stdv;
    }

    st.hmm.update(obs);
    st.warming_up = false;

    const HmmFilter::State dominant = st.hmm.dominant();
    const double confidence = st.hmm.confidence();

    if (dominant == st.regime) {
        ++st.regime_dwell;
        return;
    }

    // Commit to new regime only after sufficient confidence and dwell time.
    if (confidence < confidence_threshold_ || st.regime_dwell < min_dwell_bars_)
        return;

    ygg::log::info("[HMM] {} regime {} → {} (conf={:.3f} α=[{:.2f},{:.2f},{:.2f},{:.2f}] dwell={})",
                   st.symbol,
                   HmmFilter::state_name(st.regime),
                   HmmFilter::state_name(dominant),
                   confidence,
                   st.hmm.alpha()[0],
                   st.hmm.alpha()[1],
                   st.hmm.alpha()[2],
                   st.hmm.alpha()[3],
                   st.regime_dwell);

    begin_transition(st, dominant);
}

// ── Regime transitions ────────────────────────────────────────────────────────

void HmmStrategy::begin_transition(InstrumentState& st, HmmFilter::State target) {
    st.target_regime = target;
    st.transitioning = true;
    st.closing_position = false;
    st.close_order_id = 0;
    st.transition_start_ns = st.last_bbo_ns;
    st.pending_cancels.clear();

    auto cancel_tracked = [&](uint64_t& oid) {
        if (oid != 0) {
            cancel_order(st, oid);
            st.pending_cancels.insert(oid);
            oid = 0;
        }
    };

    cancel_tracked(st.momentum_order_id);
    cancel_tracked(st.reversion_order_id);
    cancel_tracked(st.mm_bid_id);
    cancel_tracked(st.mm_ask_id);

    if (st.pending_cancels.empty())
        check_transition_complete(st);
}

void HmmStrategy::check_transition_complete(InstrumentState& st) {
    if (!st.pending_cancels.empty())
        return;
    if (st.closing_position)
        return;

    const int64_t net = positions_.net_qty(st.instrument_id, st.exchange_id);
    if (net != 0) {
        const OrderSide::Value close_side = (net > 0) ? OrderSide::SELL : OrderSide::BUY;
        const double price = (close_side == OrderSide::BUY)
                                 ? round_price(st, st.ask * (1.0 + aggress_bps_ / 10000.0), true)
                                 : round_price(st, st.bid * (1.0 - aggress_bps_ / 10000.0), false);

        st.close_order_id = send_order(st,
                                       close_side,
                                       OrderType::LIMIT,
                                       TimeInForce::IOC,
                                       price,
                                       static_cast<double>(std::abs(net)) / kQtyScale);
        if (st.close_order_id != 0) {
            st.closing_position = true;
            ygg::log::info("[HMM] {} closing position (net={}) before entering {}",
                           st.symbol,
                           net,
                           HmmFilter::state_name(st.target_regime));
            return;
        }
    }

    // Transition complete.
    st.regime = st.target_regime;
    st.transitioning = false;
    st.closing_position = false;
    st.regime_dwell = 0;
    st.has_momentum_position = false;
    st.has_reversion_position = false;

    ygg::log::info("[HMM] {} now in regime {}", st.symbol, HmmFilter::state_name(st.regime));

    if (st.regime == HmmFilter::State::HIGH_VOL)
        mm_quote(st);
}

// ── Momentum sub-strategy ────────────────────────────────────────────────────

void HmmStrategy::momentum_update_ema(InstrumentState& st, double close) {
    if (st.ema_warmup == 0) {
        st.ema_fast = st.ema_slow = close;
        st.ema_warmup = 1;
    } else {
        const double af = 2.0 / (ema_fast_period_ + 1.0);
        const double as = 2.0 / (ema_slow_period_ + 1.0);
        st.ema_fast = af * close + (1.0 - af) * st.ema_fast;
        st.ema_slow = as * close + (1.0 - as) * st.ema_slow;
        ++st.ema_warmup;
    }
}

void HmmStrategy::momentum_check_signal(InstrumentState& st) {
    if (st.ema_warmup < ema_slow_period_)
        return;
    if (st.bar_atr_warmup < atr_period_ || st.bar_atr < 1e-12)
        return;
    if (st.last_bbo_ns < st.reject_cooldown_until_ns)
        return;

    const double mid = (st.bid + st.ask) * 0.5;
    const bool trend_up = (st.regime == HmmFilter::State::TRENDING_UP);

    // Require EMA direction to agree with HMM regime.
    if (trend_up && st.ema_fast <= st.ema_slow)
        return;
    if (!trend_up && st.ema_fast >= st.ema_slow)
        return;

    const OrderSide::Value side = trend_up ? OrderSide::BUY : OrderSide::SELL;
    const double price = trend_up ? round_price(st, st.ask * (1.0 + aggress_bps_ / 10000.0), true)
                                  : round_price(st, st.bid * (1.0 - aggress_bps_ / 10000.0), false);
    const double qty = round_qty(st, momentum_qty_usd_, mid);
    if (qty <= 0.0)
        return;

    st.momentum_side = side;
    st.momentum_order_id = send_order(st, side, OrderType::LIMIT, TimeInForce::IOC, price, qty);

    ygg::log::info("[HMM] {} momentum {} signal: ema_fast={:.4f} ema_slow={:.4f} atr={:.4f}",
                   st.symbol,
                   trend_up ? "BUY" : "SELL",
                   st.ema_fast,
                   st.ema_slow,
                   st.bar_atr);
}

void HmmStrategy::momentum_check_exit(InstrumentState& st, double mid) {
    if (!st.has_momentum_position || st.momentum_order_id != 0)
        return;

    bool should_exit = false;
    const char* reason = nullptr;

    if (st.momentum_side == OrderSide::BUY) {
        if (mid <= st.momentum_stop) {
            should_exit = true;
            reason = "stop_loss";
        } else if (mid >= st.momentum_target) {
            should_exit = true;
            reason = "take_profit";
        } else if (st.ema_fast < st.ema_slow) {
            should_exit = true;
            reason = "ema_reversal";
        }
    } else {
        if (mid >= st.momentum_stop) {
            should_exit = true;
            reason = "stop_loss";
        } else if (mid <= st.momentum_target) {
            should_exit = true;
            reason = "take_profit";
        } else if (st.ema_fast > st.ema_slow) {
            should_exit = true;
            reason = "ema_reversal";
        }
    }

    if (should_exit) {
        ygg::log::info("[HMM] {} momentum exit: {} mid={:.2f} stop={:.2f} target={:.2f}",
                       st.symbol,
                       reason,
                       mid,
                       st.momentum_stop,
                       st.momentum_target);
        momentum_close_position(st);
    }
}

void HmmStrategy::momentum_close_position(InstrumentState& st) {
    const int64_t net = positions_.net_qty(st.instrument_id, st.exchange_id);
    if (net == 0) {
        st.has_momentum_position = false;
        return;
    }

    const OrderSide::Value side = (net > 0) ? OrderSide::SELL : OrderSide::BUY;
    const double price = (side == OrderSide::BUY) ? round_price(st, st.ask * (1.0 + aggress_bps_ / 10000.0), true)
                                                  : round_price(st, st.bid * (1.0 - aggress_bps_ / 10000.0), false);

    st.momentum_order_id =
        send_order(st, side, OrderType::LIMIT, TimeInForce::IOC, price, static_cast<double>(std::abs(net)) / kQtyScale);
}

// ── VWAP reversion sub-strategy ──────────────────────────────────────────────

void HmmStrategy::reversion_check_signal(InstrumentState& st) {
    if (st.bar_atr_warmup < atr_period_ || st.bar_atr < 1e-12)
        return;
    if (st.last_bbo_ns < st.reject_cooldown_until_ns)
        return;
    if (st.vwap_den_sum <= 0.0)
        return;

    const double mid = (st.bid + st.ask) * 0.5;
    const double wvap = rolling_vwap(st);
    const double deviation = mid - wvap;
    const double threshold = vwap_deviation_atr_ * st.bar_atr;

    if (std::abs(deviation) < threshold)
        return;

    const OrderSide::Value side = (deviation > 0.0) ? OrderSide::SELL : OrderSide::BUY;
    const double price = (side == OrderSide::SELL) ? round_price(st, st.bid * (1.0 - aggress_bps_ / 10000.0), false)
                                                   : round_price(st, st.ask * (1.0 + aggress_bps_ / 10000.0), true);
    const double qty = round_qty(st, reversion_qty_usd_, mid);
    if (qty <= 0.0)
        return;

    st.reversion_side = side;
    st.reversion_stop =
        (side == OrderSide::SELL) ? mid + vwap_stop_atr_ * st.bar_atr : mid - vwap_stop_atr_ * st.bar_atr;
    st.reversion_order_id = send_order(st, side, OrderType::LIMIT, TimeInForce::IOC, price, qty);

    ygg::log::info(
        "[HMM] {} vwap reversion {}: mid={:.4f} vwap={:.4f} dev={:.4f} "
        "thr={:.4f} stop={:.4f}",
        st.symbol,
        side == OrderSide::SELL ? "SELL" : "BUY",
        mid,
        wvap,
        deviation,
        threshold,
        st.reversion_stop);
}

void HmmStrategy::reversion_check_exit(InstrumentState& st, double mid) {
    if (!st.has_reversion_position || st.reversion_order_id != 0)
        return;
    if (st.vwap_den_sum <= 0.0)
        return;

    const double wvap = rolling_vwap(st);
    const double deviation = mid - wvap;
    const double close_thr = vwap_close_atr_ * st.bar_atr;

    bool should_exit = false;
    const char* reason = nullptr;

    if (st.reversion_side == OrderSide::SELL) {
        if (deviation <= close_thr) {
            should_exit = true;
            reason = "reverted";
        } else if (mid >= st.reversion_stop) {
            should_exit = true;
            reason = "stop_loss";
        }
    } else {
        if (deviation >= -close_thr) {
            should_exit = true;
            reason = "reverted";
        } else if (mid <= st.reversion_stop) {
            should_exit = true;
            reason = "stop_loss";
        }
    }

    if (should_exit) {
        ygg::log::info("[HMM] {} reversion exit: {} mid={:.4f} vwap={:.4f}", st.symbol, reason, mid, wvap);
        reversion_close_position(st);
    }
}

void HmmStrategy::reversion_close_position(InstrumentState& st) {
    const int64_t net = positions_.net_qty(st.instrument_id, st.exchange_id);
    if (net == 0) {
        st.has_reversion_position = false;
        return;
    }

    const OrderSide::Value side = (net > 0) ? OrderSide::SELL : OrderSide::BUY;
    const double price = (side == OrderSide::BUY) ? round_price(st, st.ask * (1.0 + aggress_bps_ / 10000.0), true)
                                                  : round_price(st, st.bid * (1.0 - aggress_bps_ / 10000.0), false);

    st.reversion_order_id =
        send_order(st, side, OrderType::LIMIT, TimeInForce::IOC, price, static_cast<double>(std::abs(net)) / kQtyScale);
}

// ── Market making (Avellaneda-Stoikov) ────────────────────────────────────────

void HmmStrategy::mm_quote(InstrumentState& st) {
    if (!order_mgr_)
        return;
    if (st.mm_last_quote_ns > 0 && st.last_bbo_ns - st.mm_last_quote_ns < mm_requote_ns_)
        return;

    const double mid = (st.bid + st.ask) * 0.5;
    if (mid <= 0.0 || st.ewma_var <= 0.0)
        return;

    // Annualise EWMA vol for A-S: σ_T = σ_bar × sqrt(bars_per_year) × sqrt(T_years).
    const double bar_s = bar_interval_ns_ / 1e9;
    const double bars_per_year = 365.0 * 24.0 * 3600.0 / bar_s;
    const double sigma_annual = std::sqrt(st.ewma_var * bars_per_year);
    const double sigma_T = sigma_annual * std::sqrt(mm_horizon_s_ / (365.0 * 24.0 * 3600.0));
    const double sigma_T_sq = sigma_T * sigma_T;

    // Signed inventory in base-currency units.
    const double q = static_cast<double>(positions_.net_qty(st.instrument_id, st.exchange_id)) / kQtyScale;

    // A-S reservation price and half-spread.
    const double reservation = mid - mm_gamma_ * sigma_T_sq * q;
    double half_spread = mm_gamma_ * sigma_T_sq * 0.5 + (1.0 / mm_gamma_) * std::log(1.0 + mm_gamma_ / mm_k_);

    // Floor at fee coverage.
    const double fee_floor = mid * mm_min_spread_bps_ / 10000.0;
    if (half_spread < fee_floor)
        half_spread = fee_floor;

    const double new_bid = round_price(st, reservation - half_spread, false);
    const double new_ask = round_price(st, reservation + half_spread, true);
    if (new_bid <= 0.0 || new_ask <= new_bid)
        return;

    // Skip requote if prices haven't moved by more than one tick.
    const double tick = st.tick_size > 0.0 ? st.tick_size : mid * 1e-4;
    if (st.mm_bid_id != 0 && std::abs(new_bid - st.mm_bid_price) < tick && st.mm_ask_id != 0 &&
        std::abs(new_ask - st.mm_ask_price) < tick)
        return;

    mm_cancel_quotes(st);

    const double qty = round_qty(st, mm_qty_usd_, mid);
    if (qty <= 0.0)
        return;

    const double inv_usd = q * mid;
    if (inv_usd < mm_max_inventory_usd_) {
        st.mm_bid_id = send_order(st, OrderSide::BUY, OrderType::LIMIT, TimeInForce::GTC, new_bid, qty);
        st.mm_bid_price = new_bid;
    }
    if (inv_usd > -mm_max_inventory_usd_) {
        st.mm_ask_id = send_order(st, OrderSide::SELL, OrderType::LIMIT, TimeInForce::GTC, new_ask, qty);
        st.mm_ask_price = new_ask;
    }

    st.mm_last_quote_ns = st.last_bbo_ns;
    ygg::log::debug("[HMM] {} mm: bid={:.4f} ask={:.4f} res={:.4f} hs={:.4f} q={:.6f} σ_T={:.6f}",
                    st.symbol,
                    new_bid,
                    new_ask,
                    reservation,
                    half_spread,
                    q,
                    sigma_T);
}

void HmmStrategy::mm_cancel_quotes(InstrumentState& st) {
    if (st.mm_bid_id != 0) {
        cancel_order(st, st.mm_bid_id);
        st.mm_bid_id = 0;
    }
    if (st.mm_ask_id != 0) {
        cancel_order(st, st.mm_ask_id);
        st.mm_ask_id = 0;
    }
}

// ── Exec reports ─────────────────────────────────────────────────────────────

void HmmStrategy::on_exec_report(const bpt::messages::ExecutionReport& rpt) {
    const uint64_t order_id = rpt.orderId();
    auto inst_it = order_to_instrument_.find(order_id);
    if (inst_it == order_to_instrument_.end())
        return;

    auto st_it = state_.find(inst_it->second);
    if (st_it == state_.end())
        return;

    InstrumentState& st = st_it->second;
    const auto status = rpt.status();

    if (status == ExecStatus::ACKED)
        return;

    if (status == ExecStatus::REJECTED) {
        ygg::log::warn("[HMM] {} order_id={} REJECTED reason={} source={}",
                       st.symbol,
                       order_id,
                       bpt::messages::RejectReason::c_str(rpt.rejectReason()),
                       bpt::messages::RejectSource::c_str(rpt.rejectSource()));
    } else {
        ygg::log::info("[HMM] {} order_id={} {} filled={:.6f}@{:.4f}",
                       st.symbol,
                       order_id,
                       bpt::messages::ExecStatus::c_str(status),
                       static_cast<double>(rpt.filledQty()) / kQtyScale,
                       static_cast<double>(rpt.price()) / kPriceScale);
    }

    if (status == ExecStatus::FILLED || status == ExecStatus::PARTIAL)
        positions_.on_fill(st.instrument_id, st.exchange_id, rpt.side(), rpt.filledQty(), rpt.price());

    const bool is_terminal =
        (status == ExecStatus::FILLED || status == ExecStatus::CANCELLED || status == ExecStatus::REJECTED);
    if (!is_terminal)
        return;

    // Rejection backoff — exchange rejects only, not risk/gateway.
    if (status == ExecStatus::REJECTED) {
        const auto src = rpt.rejectSource();
        const bool gw_reject = (src == RejectSource::GATEWAY || src == RejectSource::RISK);
        if (!gw_reject) {
            if (++st.consecutive_rejects >= 3) {
                st.reject_cooldown_until_ns = st.last_bbo_ns + 30'000'000'000ULL;
                ygg::log::warn("[HMM] {} 30s reject cooldown after {} consecutive rejects",
                               st.symbol,
                               st.consecutive_rejects);
            }
        }
    } else {
        st.consecutive_rejects = 0;
    }

    // ── Transition handling ──
    if (st.transitioning) {
        st.pending_cancels.erase(order_id);

        if (st.closing_position && order_id == st.close_order_id) {
            st.close_order_id = 0;
            st.closing_position = false;
        }
        if (st.pending_cancels.empty() && !st.closing_position)
            check_transition_complete(st);
    }
    // ── Momentum ──
    else if (st.regime == HmmFilter::State::TRENDING_UP || st.regime == HmmFilter::State::TRENDING_DOWN) {
        if (order_id == st.momentum_order_id) {
            if (status == ExecStatus::FILLED) {
                if (st.has_momentum_position) {
                    st.has_momentum_position = false;
                    st.momentum_order_id = 0;
                    ygg::log::info("[HMM] {} momentum position closed", st.symbol);
                } else {
                    st.has_momentum_position = true;
                    const double entry = static_cast<double>(rpt.price()) / kPriceScale;
                    st.momentum_entry = entry;
                    if (st.momentum_side == OrderSide::BUY) {
                        st.momentum_stop = entry - atr_stop_mult_ * st.bar_atr;
                        st.momentum_target = entry + atr_target_mult_ * st.bar_atr;
                    } else {
                        st.momentum_stop = entry + atr_stop_mult_ * st.bar_atr;
                        st.momentum_target = entry - atr_target_mult_ * st.bar_atr;
                    }
                    ygg::log::info("[HMM] {} momentum entered {} entry={:.4f} stop={:.4f} target={:.4f}",
                                   st.symbol,
                                   st.momentum_side == OrderSide::BUY ? "LONG" : "SHORT",
                                   entry,
                                   st.momentum_stop,
                                   st.momentum_target);
                }
            } else {
                st.momentum_order_id = 0;
            }
        }
    }
    // ── VWAP reversion ──
    else if (st.regime == HmmFilter::State::MEAN_REVERT) {
        if (order_id == st.reversion_order_id) {
            if (status == ExecStatus::FILLED) {
                if (st.has_reversion_position) {
                    st.has_reversion_position = false;
                    st.reversion_order_id = 0;
                    ygg::log::info("[HMM] {} reversion position closed", st.symbol);
                } else {
                    st.has_reversion_position = true;
                    ygg::log::info("[HMM] {} reversion entered", st.symbol);
                }
            } else {
                st.reversion_order_id = 0;
            }
        }
    }
    // ── Market making ──
    else if (st.regime == HmmFilter::State::HIGH_VOL) {
        if (order_id == st.mm_bid_id) {
            if (status == ExecStatus::FILLED)
                ygg::log::info("[HMM] {} mm bid filled @ {:.4f}",
                               st.symbol,
                               static_cast<double>(rpt.price()) / kPriceScale);
            st.mm_bid_id = 0;
        } else if (order_id == st.mm_ask_id) {
            if (status == ExecStatus::FILLED)
                ygg::log::info("[HMM] {} mm ask filled @ {:.4f}",
                               st.symbol,
                               static_cast<double>(rpt.price()) / kPriceScale);
            st.mm_ask_id = 0;
        }
    }

    order_to_instrument_.erase(order_id);
}

// ── Utility ───────────────────────────────────────────────────────────────────

double HmmStrategy::rolling_vwap(const InstrumentState& st) const {
    if (st.vwap_den_sum <= 0.0)
        return (st.bid + st.ask) * 0.5;
    return st.vwap_num_sum / st.vwap_den_sum;
}

double HmmStrategy::round_qty(const InstrumentState& st, double qty_usd, double mid) const {
    if (mid <= 0.0)
        return 0.0;
    const double lot = st.lot_size > 0.0 ? st.lot_size : 1.0 / kQtyScale;
    return std::floor(qty_usd / mid / lot) * lot;
}

double HmmStrategy::round_price(const InstrumentState& st, double price, bool round_up) const {
    if (st.tick_size <= 0.0)
        return price;
    return round_up ? std::ceil(price / st.tick_size) * st.tick_size : std::floor(price / st.tick_size) * st.tick_size;
}

uint64_t HmmStrategy::send_order(InstrumentState& st,
                                 bpt::messages::OrderSide::Value side,
                                 bpt::messages::OrderType::Value type,
                                 bpt::messages::TimeInForce::Value tif,
                                 double price,
                                 double qty) {
    if (!order_mgr_)
        return 0;

    const uint64_t order_id = order_mgr_->place_order(st.instrument_id, st.exchange_id, side, type, tif, price, qty);
    if (order_id == 0)
        return 0;

    order_to_instrument_[order_id] = st.instrument_id;
    ygg::log::info("[HMM] {} {} {} @ {:.4f} qty={:.6f} → order_id={}",
                   st.symbol,
                   side == OrderSide::BUY ? "BUY" : "SELL",
                   tif == TimeInForce::GTC ? "GTC" : "IOC",
                   price,
                   qty,
                   order_id);
    return order_id;
}

void HmmStrategy::cancel_order(InstrumentState& st, uint64_t order_id) {
    if (!order_mgr_ || order_id == 0)
        return;
    order_mgr_->cancel_order(order_id, st.exchange_id, st.instrument_id);
}

}  // namespace bpt::strategy::strategy
