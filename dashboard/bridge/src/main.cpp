// bridge — subscribes to Aeron MD + exec report streams, broadcasts JSON
// messages over WebSocket to the dashboard frontend.

#include <analytics/messaging/toxicity_update.h>

#include "bridge/account_subscriber.h"
#include "bridge/exec_subscriber.h"
#include "bridge/md_subscriber.h"
#include "bridge/message_encoder.h"
#include "bridge/position_tracker.h"
#include "bridge/settings.h"
#include "bridge/ws_server.h"

#include <Aeron.h>
#include <chrono>
#include <string>
#include <thread>
#include <yggdrasil/aeron/aeron_utils.h>
#include <yggdrasil/logging.h>
#include <yggdrasil/signal.h>

int main(int argc, char** argv) {
    ygg::signal::install();

    std::string config_path = "config/bridge.toml";
    std::string strategy_override;          // --strategy-name    → session.strategy
    std::string symbol_override;            // --symbol           → session.symbol
    std::string exchange_override;          // --exchange         → session.exchange
    std::string mode_override;              // --mode             → session.mode
    std::string instrument_type_override;   // --instrument-type  → session.instrument_type
    uint64_t    instrument_id_override = 0; // --instrument-id    → session.instrument_id

    for (int i = 1; i < argc - 1; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--config")            config_path             = argv[i + 1];
        if (arg == "--strategy-name")     strategy_override       = argv[i + 1];
        if (arg == "--symbol")            symbol_override         = argv[i + 1];
        if (arg == "--exchange")          exchange_override       = argv[i + 1];
        if (arg == "--mode")              mode_override           = argv[i + 1];
        if (arg == "--instrument-type")   instrument_type_override = argv[i + 1];
        if (arg == "--instrument-id") {
            try { instrument_id_override = std::stoull(argv[i + 1]); }
            catch (const std::exception&) { /* ignore, default stays */ }
        }
    }

    bridge::config::Settings settings;
    try {
        settings = bridge::config::load(config_path);
    } catch (const std::exception& e) {
        ygg::logging::init("bridge");
        ygg::log::error("Failed to load config: {}", e.what());
        return 1;
    }

    // CLI overrides take precedence over TOML values.
    if (!strategy_override.empty())        settings.strategy         = strategy_override;
    if (!symbol_override.empty())          settings.symbol           = symbol_override;
    if (!exchange_override.empty())        settings.exchange         = exchange_override;
    if (!mode_override.empty())            settings.mode             = mode_override;
    if (!instrument_type_override.empty()) settings.instrument_type  = instrument_type_override;
    if (instrument_id_override > 0)        settings.instrument_id    = instrument_id_override;

    ygg::logging::init("bridge", settings.logging);
    ygg::log::info("bridge starting — ws :{}  aeron {}", settings.ws_port, settings.media_driver_dir);
    ygg::log::info("[bridge] md_data stream={}  exec_report stream={}  control stream={}",
                   settings.md_data.stream_id, settings.exec_report.stream_id,
                   settings.control_command.stream_id);
    ygg::log::info("[bridge] mode={} strategy={} symbol={}@{} instrument_filter={}",
                   settings.mode, settings.strategy, settings.symbol, settings.exchange,
                   settings.instrument_id == 0 ? "(none)" : std::to_string(settings.instrument_id));

    // ── Aeron ────────────────────────────────────────────────────────────────
    auto aeron = ygg::aeron::connect(settings.media_driver_dir);

    bridge::MdSubscriber      md_sub(aeron, settings.md_data.channel, settings.md_data.stream_id);
    bridge::ExecSubscriber    exec_sub(aeron, settings.exec_report.channel, settings.exec_report.stream_id);
    bridge::AccountSubscriber account_sub(aeron, settings.account_snapshot.channel, settings.account_snapshot.stream_id);

    // ── Portfolio snapshot subscription (Strategy → bridge) ────────────────────
    // Strategy publishes JSON at ~10Hz with option legs, Greeks, and vol surface.
    // The bridge relays it as-is to all WS clients.
    std::shared_ptr<aeron::Subscription> snapshot_sub;
    if (settings.portfolio_snapshot.stream_id != 0) {
        const int64_t reg_id = aeron->addSubscription(
            settings.portfolio_snapshot.channel, settings.portfolio_snapshot.stream_id);
        for (int i = 0; i < 500; ++i) {
            snapshot_sub = aeron->findSubscription(reg_id);
            if (snapshot_sub) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (snapshot_sub) {
            ygg::log::info("[bridge] portfolio snapshot subscription ready on stream {}",
                           settings.portfolio_snapshot.stream_id);
        } else {
            ygg::log::warn("[bridge] portfolio snapshot subscription unavailable");
        }
    }

    // ── Analytics toxicity subscription (optional) ──────────────────────────────────
    std::shared_ptr<aeron::Subscription> tyr_sub;
    if (settings.toxicity.stream_id != 0) {
        const int64_t reg_id = aeron->addSubscription(
            settings.toxicity.channel, settings.toxicity.stream_id);
        for (int i = 0; i < 500; ++i) {
            tyr_sub = aeron->findSubscription(reg_id);
            if (tyr_sub) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (tyr_sub) {
            ygg::log::info("[bridge] tyr toxicity subscription ready on stream {}",
                           settings.toxicity.stream_id);
        } else {
            ygg::log::warn("[bridge] tyr toxicity subscription unavailable");
        }
    }

    // ── Control publication (bridge → Strategy) ────────────────────────────────
    // 1-byte command: 0x00 = HALT, 0x01 = RESUME.  No SBE — this is a
    // lightweight control path, not a high-throughput data stream.
    std::shared_ptr<aeron::Publication> ctrl_pub;
    if (settings.control_command.stream_id != 0) {
        const int64_t reg_id = aeron->addPublication(
            settings.control_command.channel, settings.control_command.stream_id);
        for (int i = 0; i < 500; ++i) {
            ctrl_pub = aeron->findPublication(reg_id);
            if (ctrl_pub) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (ctrl_pub) {
            ygg::log::info("[bridge] control publication ready on stream {}",
                           settings.control_command.stream_id);
        } else {
            ygg::log::warn("[bridge] control publication unavailable — halt/resume disabled");
        }
    }

    // ── WebSocket server ─────────────────────────────────────────────────────
    bridge::WsServer ws(settings.ws_port);

    ws.on_command = [&](const std::string& cmd) {
        uint8_t ctrl_byte;
        std::string status_str;

        if (cmd == "halt") {
            ctrl_byte = 0x00;
            status_str = "halted";
        } else if (cmd == "resume") {
            ctrl_byte = 0x01;
            status_str = "live";
        } else {
            ygg::log::warn("[bridge] unknown command: {}", cmd);
            return;
        }

        // Publish to Strategy via Aeron
        if (ctrl_pub) {
            aeron::AtomicBuffer buf(reinterpret_cast<uint8_t*>(&ctrl_byte), 1);
            auto result = ctrl_pub->offer(buf, 0, 1);
            if (result < 0) {
                ygg::log::warn("[bridge] control offer failed: {}", result);
            }
        }

        // Broadcast status to all connected dashboard clients
        ws.publish(bridge::MsgKind::Status, bridge::encode::status(status_str));
        ygg::log::info("[bridge] command '{}' → status '{}'", cmd, status_str);
    };

    ws.start();

    ws.publish(bridge::MsgKind::Session,
               bridge::encode::session(settings.symbol,
                                       settings.strategy,
                                       settings.exchange,
                                       settings.mode,
                                       settings.instrument_type));
    ws.publish(bridge::MsgKind::Status, bridge::encode::status("live"));

    // ── Position state (bridge is authoritative) ─────────────────────────────
    // Tracker baseline is 0; per-fill `equity` = cumulative realized PnL since
    // session start. Absolute equity is sourced from order-gateway AccountSnapshots
    // (see account_subscriber + the dashboard's accountHistory).
    bridge::PositionTracker tracker;
    double last_mid = 0.0;

    // Tick throttle — BBO mids update ~1000 Hz but the dashboard only needs
    // ~30 Hz for a smooth visual.  Drop intermediate ticks; the most-recent
    // value always wins.  Fills and position messages bypass the throttle so
    // every trade is delivered instantly.
    using clock = std::chrono::steady_clock;
    constexpr auto kTickMinInterval = std::chrono::milliseconds(33);  // ~30 Hz
    auto last_tick_bcast = clock::now() - std::chrono::seconds(1);

    // ── Handlers ─────────────────────────────────────────────────────────────
    //
    // When settings.instrument_id is non-zero, MD ticks and fills for other
    // instruments are dropped before they reach the position tracker or the
    // broadcast queue.  This is the cleanest way to run the dashboard for a
    // single-instrument view of a multi-instrument strategy.

    md_sub.set_handler([&](uint64_t instr, double mid, uint64_t ts_ns) {
        if (settings.instrument_id != 0 && instr != settings.instrument_id) return;
        last_mid = mid;
        const auto now = clock::now();
        if (now - last_tick_bcast < kTickMinInterval) return;
        last_tick_bcast = now;
        ws.publish(bridge::MsgKind::Tick, bridge::encode::tick(ts_ns, settings.symbol, mid));
    });

    // Order lifecycle handler — forwards all exec report statuses to the
    // dashboard so it can display open/working orders.
    exec_sub.set_order_handler([&](const bridge::ExecSubscriber::OrderEvent& ev) {
        if (settings.instrument_id != 0 && ev.instrument_id != settings.instrument_id) return;

        static const char* kStatusStr[] = {"acked", "filled", "partial", "rejected", "cancelled"};
        static const char* kTypeStr[]   = {"MARKET", "LIMIT", "POST_ONLY"};

        const char* status_s = ev.status < 5 ? kStatusStr[ev.status] : "unknown";
        const char* type_s   = ev.order_type < 3 ? kTypeStr[ev.order_type] : "UNKNOWN";

        ws.publish(bridge::MsgKind::Order,
                   bridge::encode::order(ev.ts_ns,
                                         ev.order_id,
                                         settings.symbol,
                                         ev.side,
                                         type_s,
                                         ev.price,
                                         ev.qty,
                                         ev.filled_qty,
                                         ev.remaining_qty,
                                         status_s));
    });

    // Account snapshot handler — forwards live exchange balance/equity to the
    // frontend. The dashboard uses these as the canonical equity baseline so
    // the equity curve reflects the actual exchange account, not a static
    // starting_capital config value.
    account_sub.set_handler([&](const bridge::AccountSubscriber::Snapshot& s) {
        std::vector<bridge::encode::AccountPosition> positions;
        positions.reserve(s.positions.size());
        for (const auto& p : s.positions) {
            positions.push_back({p.exchange_symbol, p.net_qty, p.avg_entry, p.unrealized_pnl});
        }
        ws.publish(bridge::MsgKind::Order,
                   bridge::encode::account(s.ts_ns, s.available_balance, s.total_equity, positions));
    });

    exec_sub.set_handler([&](const bridge::ExecSubscriber::Fill& f) {
        if (settings.instrument_id != 0 && f.instrument_id != settings.instrument_id) return;

        const auto res = tracker.apply(f.side, f.qty, f.price);

        static const char* kTypeStr[] = {"MARKET", "LIMIT", "POST_ONLY"};
        const char* type_s = f.order_type < 3 ? kTypeStr[f.order_type] : "UNKNOWN";
        ws.publish(bridge::MsgKind::Fill,
                   bridge::encode::fill(f.ts_ns,
                                        f.order_id,
                                        settings.symbol,
                                        f.side,
                                        type_s,
                                        f.qty,
                                        f.price,
                                        f.fee,
                                        res.realized_pnl,
                                        res.cumulative_pnl));

        const double unreal = last_mid > 0 ? tracker.unrealized_pnl(last_mid) : 0.0;
        ws.publish(bridge::MsgKind::Position,
                   bridge::encode::position(settings.symbol, res.net_qty, res.avg_entry, unreal));
    });

    // ── Poll loop ────────────────────────────────────────────────────────────
    while (ygg::signal::is_running()) {
        int work = 0;
        work += md_sub.poll(32);
        work += exec_sub.poll(32);
        work += account_sub.poll(8);

        // Poll portfolio snapshots from fenrir and relay as-is to WS clients.
        // The JSON from fenrir already has type:"portfolio" so the frontend
        // can dispatch it directly.
        if (snapshot_sub) {
            work += snapshot_sub->poll(
                [&ws](aeron::AtomicBuffer& buffer,
                      aeron::util::index_t offset,
                      aeron::util::index_t length,
                      aeron::Header& /*hdr*/) {
                    std::string json(
                        reinterpret_cast<const char*>(buffer.buffer() + offset),
                        static_cast<std::size_t>(length));
                    ws.publish(bridge::MsgKind::Order, std::move(json));
                },
                1);
        }

        // Poll tyr toxicity updates and relay to frontend.
        if (tyr_sub) {
            work += tyr_sub->poll(
                [&ws](aeron::AtomicBuffer& buffer,
                      aeron::util::index_t offset,
                      aeron::util::index_t length,
                      aeron::Header& /*hdr*/) {
                    if (static_cast<std::size_t>(length) != sizeof(bpt::analytics::messaging::ToxicityUpdate))
                        return;
                    bpt::analytics::messaging::ToxicityUpdate u;
                    std::memcpy(&u, buffer.buffer() + offset, sizeof(u));
                    double bid_m = u.bid_markout_5s_bps, ask_m = u.ask_markout_5s_bps;
                    double bid_ar = u.bid_adverse_rate, ask_ar = u.ask_adverse_rate;
                    uint32_t bid_n = u.bid_sample_count, ask_n = u.ask_sample_count;
                    double bid_t = u.bid_toxicity_score, ask_t = u.ask_toxicity_score;
                    double bid_fr = u.bid_fill_rate, ask_fr = u.ask_fill_rate;
                    double bid_ttf = u.bid_ttf_ms, ask_ttf = u.ask_ttf_ms;
                    ws.publish(bridge::MsgKind::Toxicity,
                               bridge::encode::toxicity(bid_m, ask_m, bid_ar, ask_ar,
                                                        bid_n, ask_n, bid_t, ask_t,
                                                        bid_fr, ask_fr, bid_ttf, ask_ttf));
                },
                4);
        }

        if (work == 0) std::this_thread::sleep_for(std::chrono::microseconds(500));
    }

    ygg::log::info("bridge shutting down");
    ws.publish(bridge::MsgKind::Status, bridge::encode::status("off"));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ws.stop();
    return 0;
}
