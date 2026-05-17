#include "bridge/app/bridge_service.h"

#include "bridge/ws/message_encoder.h"
#include "bridge/ws/msg_kind.h"

#include <bpt_common/logging.h>
#include <bpt_common/signal.h>

#include <chrono>
#include <thread>
#include <utility>
#include <vector>

namespace bpt::bridge {

constexpr std::chrono::milliseconds BridgeService::kTickMinInterval;

BridgeService::BridgeService(config::Settings settings,
                             messaging::BridgeBus bus,
                             std::shared_ptr<ws::IBroadcaster> broadcaster,
                             std::shared_ptr<messaging::api::DashboardControlPublisher> ctrl_sink)
    : settings_(std::move(settings)),
      bus_(std::move(bus)),
      broadcaster_(std::move(broadcaster)),
      ctrl_sink_(std::move(ctrl_sink)) {}

void BridgeService::publish_session_init() {
    if (!broadcaster_)
        return;
    broadcaster_->publish(MsgKind::Session,
                          encode::session(settings_.symbol,
                                          settings_.strategy,
                                          settings_.exchange,
                                          settings_.mode,
                                          settings_.instrument_type));
    broadcaster_->publish(MsgKind::Status, encode::status("live"));
}

// ── Event handlers ────────────────────────────────────────────────────────────

void BridgeService::on_md_tick(uint64_t instrument_id, double mid, uint64_t ts_ns) {
    if (settings_.instrument_id != 0 && instrument_id != settings_.instrument_id)
        return;
    last_mid_ = mid;

    const auto now = std::chrono::steady_clock::now();
    if (now - last_tick_bcast_ < kTickMinInterval)
        return;
    last_tick_bcast_ = now;

    if (broadcaster_)
        broadcaster_->publish(MsgKind::Tick, encode::tick(ts_ns, settings_.symbol, mid));
}

void BridgeService::on_exec_fill(const messaging::api::ExecSubscriber::Fill& f) {
    if (settings_.instrument_id != 0 && f.instrument_id != settings_.instrument_id)
        return;

    const auto res = tracker_.apply(f.side, f.qty, f.price);

    static const char* kTypeStr[] = {"MARKET", "LIMIT"};
    const char* type_s = f.order_type < 2 ? kTypeStr[f.order_type] : "UNKNOWN";

    if (broadcaster_) {
        broadcaster_->publish(MsgKind::Fill,
                              encode::fill(f.ts_ns,
                                           f.order_id,
                                           settings_.symbol,
                                           f.side,
                                           type_s,
                                           f.qty,
                                           f.price,
                                           f.fee,
                                           res.realized_pnl,
                                           res.cumulative_pnl));

        const double unreal = last_mid_ > 0 ? tracker_.unrealized_pnl(last_mid_) : 0.0;
        broadcaster_->publish(MsgKind::Position,
                              encode::position(settings_.symbol, res.net_qty, res.avg_entry, unreal));
    }
}

void BridgeService::on_exec_order_event(const messaging::api::ExecSubscriber::OrderEvent& ev) {
    if (settings_.instrument_id != 0 && ev.instrument_id != settings_.instrument_id)
        return;

    static const char* kStatusStr[] = {"acked", "filled", "partial", "rejected", "cancelled"};
    static const char* kTypeStr[] = {"MARKET", "LIMIT"};

    const char* status_s = ev.status < 5 ? kStatusStr[ev.status] : "unknown";
    const char* type_s = ev.order_type < 2 ? kTypeStr[ev.order_type] : "UNKNOWN";

    if (broadcaster_)
        broadcaster_->publish(MsgKind::Order,
                              encode::order(ev.ts_ns,
                                            ev.order_id,
                                            settings_.symbol,
                                            ev.side,
                                            type_s,
                                            ev.price,
                                            ev.qty,
                                            ev.filled_qty,
                                            ev.remaining_qty,
                                            status_s));
}

void BridgeService::on_account_snapshot(const messaging::api::AccountSubscriber::Snapshot& s) {
    if (!broadcaster_)
        return;
    std::vector<encode::AccountPosition> positions;
    positions.reserve(s.positions.size());
    for (const auto& p : s.positions)
        positions.push_back({p.exchange_symbol, p.net_qty, p.avg_entry, p.unrealized_pnl});

    std::vector<encode::AccountCurrencyBalance> ccy_balances;
    ccy_balances.reserve(s.currency_balances.size());
    for (const auto& cb : s.currency_balances)
        ccy_balances.push_back({cb.ccy, cb.equity, cb.available_balance});

    broadcaster_->publish(MsgKind::Order,
                          encode::account(s.ts_ns, s.available_balance, s.total_equity, positions, ccy_balances));
}

void BridgeService::on_portfolio_json(std::string_view json) {
    if (broadcaster_)
        broadcaster_->publish(MsgKind::Order, std::string(json));
}

void BridgeService::on_toxicity(const bpt::analytics::messaging::ToxicityUpdate& u) {
    if (!broadcaster_)
        return;
    broadcaster_->publish(MsgKind::Toxicity,
                          encode::toxicity(u.bid_markout_5s_bps,
                                           u.ask_markout_5s_bps,
                                           u.bid_adverse_rate,
                                           u.ask_adverse_rate,
                                           u.bid_sample_count,
                                           u.ask_sample_count,
                                           u.bid_toxicity_score,
                                           u.ask_toxicity_score,
                                           u.bid_fill_rate,
                                           u.ask_fill_rate,
                                           u.bid_ttf_ms,
                                           u.ask_ttf_ms));
}

void BridgeService::on_market_color(const bpt::radar::messaging::MarketColor& mc) {
    if (!broadcaster_)
        return;
    encode::OptionsMarketColor opts{
        .front_expiry_yyyymmdd = mc.options_front_expiry_yyyymmdd,
        .front_time_to_expiry_y = mc.options_front_time_to_expiry_y,
        .front_forward_price = mc.options_front_forward_price,
        .front_atm_iv = mc.options_front_atm_iv,
        .front_rr_25d = mc.options_front_rr_25d,
        .front_skew_slope = mc.options_front_skew_slope,
        .back_expiry_yyyymmdd = mc.options_back_expiry_yyyymmdd,
        .back_time_to_expiry_y = mc.options_back_time_to_expiry_y,
        .back_atm_iv = mc.options_back_atm_iv,
        .term_spread = mc.options_term_spread,
        .gex = mc.options_gex,
        .max_pain_strike = mc.options_max_pain_strike,
        .total_oi = mc.options_total_oi,
        .strike_count = mc.options_strike_count,
        .expiry_count = mc.options_expiry_count,
        .strikes_with_oi = mc.options_strikes_with_oi,
    };

    encode::PerpMarketColor perp{
        .funding_rate_8h = mc.perp_funding_rate_8h,
        .next_funding_ts = mc.perp_next_funding_ts_ns,
        .basis_bps = mc.perp_basis_bps,
        .mark_price = mc.perp_mark_price,
        .index_price = mc.perp_index_price,
    };

    encode::FlowMarketColor flow{
        .buy_notional_5m = mc.flow_buy_notional_5m,
        .sell_notional_5m = mc.flow_sell_notional_5m,
        .imbalance_5m = mc.flow_imbalance_5m,
        .trade_count_5m = mc.flow_trade_count_5m,
    };

    encode::RegimeMarketColor regime{
        .realized_vol_1h = mc.regime_realized_vol_1h,
        .sample_count = mc.regime_sample_count,
    };

    // For market_color the venue can differ per underlying (BTC on Deribit,
    // ETH on Deribit, future BTC-perp on Binance), so derive from the
    // message field directly.
    static const char* kVenue[] = {"UNKNOWN", "BINANCE", "OKX", "HYPERLIQUID", "DERIBIT"};
    const char* venue = (mc.exchange_id < 5) ? kVenue[mc.exchange_id] : "UNKNOWN";

    broadcaster_->publish(MsgKind::MarketColor,
                          encode::market_color(mc.timestamp_ns, venue, mc.underlying, opts, perp, flow, regime));
}

void BridgeService::on_dashboard_command(const std::string& cmd) {
    const char* status_str = nullptr;
    if (cmd == "halt") {
        if (ctrl_sink_)
            ctrl_sink_->publish_halt();
        status_str = "halted";
    } else if (cmd == "resume") {
        if (ctrl_sink_)
            ctrl_sink_->publish_resume();
        status_str = "live";
    } else {
        bpt::common::log::warn("unknown command: {}", cmd);
        return;
    }

    if (broadcaster_)
        broadcaster_->publish(MsgKind::Status, encode::status(status_str));
    bpt::common::log::info("command '{}' → status '{}'", cmd, status_str);
}

// ── Production wiring + poll loop ─────────────────────────────────────────────

void BridgeService::run() {
    bpt::common::log::info("bridge starting — ws :{}  aeron {}", settings_.ws_port, settings_.base.media_driver_dir);
    bpt::common::log::info("md_data stream={}  exec_report stream={}  control stream={}",
                           settings_.md_data.stream_id,
                           settings_.exec_report.stream_id,
                           settings_.dashboard_control.stream_id);
    bpt::common::log::info("mode={} strategy={} symbol={}@{} instrument_filter={}",
                           settings_.mode,
                           settings_.strategy,
                           settings_.symbol,
                           settings_.exchange,
                           settings_.instrument_id == 0 ? "(none)" : std::to_string(settings_.instrument_id));

    // Wire bus callbacks to public event handlers.
    if (bus_.md_sub)
        bus_.md_sub->set_handler([this](uint64_t instr, double mid, uint64_t ts_ns) {
            on_md_tick(instr, mid, ts_ns);
        });
    if (bus_.exec_sub) {
        bus_.exec_sub->set_order_handler(
            [this](const messaging::api::ExecSubscriber::OrderEvent& ev) { on_exec_order_event(ev); });
        bus_.exec_sub->set_handler([this](const messaging::api::ExecSubscriber::Fill& f) { on_exec_fill(f); });
    }
    if (bus_.account_sub)
        bus_.account_sub->set_handler(
            [this](const messaging::api::AccountSubscriber::Snapshot& s) { on_account_snapshot(s); });
    if (bus_.portfolio_sub)
        bus_.portfolio_sub->set_handler([this](std::string_view json) { on_portfolio_json(json); });
    if (bus_.tox_sub)
        bus_.tox_sub->set_handler(
            [this](const bpt::analytics::messaging::ToxicityUpdate& u) { on_toxicity(u); });
    if (bus_.color_sub)
        bus_.color_sub->set_handler(
            [this](const bpt::radar::messaging::MarketColor& mc) { on_market_color(mc); });

    // Install dashboard command handler on the broadcaster (IO-thread-safe in
    // production WsServer; tests can drive it synchronously).
    if (broadcaster_)
        broadcaster_->set_command_handler([this](const std::string& cmd) { on_dashboard_command(cmd); });

    publish_session_init();

    // Poll loop — driven by bpt::common::signal::is_running() since
    // bpt::app::run() installed the signal handler before us.
    last_tick_bcast_ = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    while (bpt::common::signal::is_running()) {
        int work = 0;
        if (bus_.md_sub)        work += bus_.md_sub->poll(32);
        if (bus_.exec_sub)      work += bus_.exec_sub->poll(32);
        if (bus_.account_sub)   work += bus_.account_sub->poll(8);
        if (bus_.portfolio_sub) work += bus_.portfolio_sub->poll(1);
        if (bus_.tox_sub)       work += bus_.tox_sub->poll(4);
        if (bus_.color_sub)     work += bus_.color_sub->poll(4);

        if (work == 0)
            std::this_thread::sleep_for(std::chrono::microseconds(500));
    }

    // Graceful shutdown on signal — notify dashboard that bridge is going
    // away, give WS clients a beat to flush. Detach the command handler so
    // any late inbound command can't run against a half-destroyed service.
    if (broadcaster_) {
        broadcaster_->publish(MsgKind::Status, encode::status("off"));
        broadcaster_->set_command_handler(nullptr);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

}  // namespace bpt::bridge
