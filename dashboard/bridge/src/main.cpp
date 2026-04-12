// bridge — subscribes to Aeron MD + exec report streams, broadcasts JSON
// messages over WebSocket to the dashboard frontend.

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
    std::string strategy_override;      // --strategy-name    → session.strategy
    std::string symbol_override;        // --symbol           → session.symbol
    std::string exchange_override;      // --exchange         → session.exchange
    std::string mode_override;          // --mode             → session.mode
    double      starting_capital_override = 0.0;  // --starting-capital → session.starting_capital
    uint64_t    instrument_id_override    = 0;    // --instrument-id   → session.instrument_id

    for (int i = 1; i < argc - 1; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--config")            config_path        = argv[i + 1];
        if (arg == "--strategy-name")     strategy_override  = argv[i + 1];
        if (arg == "--symbol")            symbol_override    = argv[i + 1];
        if (arg == "--exchange")          exchange_override  = argv[i + 1];
        if (arg == "--mode")              mode_override      = argv[i + 1];
        if (arg == "--starting-capital") {
            try { starting_capital_override = std::stod(argv[i + 1]); }
            catch (const std::exception&) { /* ignore, default stays */ }
        }
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
    if (!strategy_override.empty())    settings.strategy          = strategy_override;
    if (!symbol_override.empty())      settings.symbol            = symbol_override;
    if (!exchange_override.empty())    settings.exchange          = exchange_override;
    if (!mode_override.empty())        settings.mode              = mode_override;
    if (starting_capital_override > 0) settings.starting_capital  = starting_capital_override;
    if (instrument_id_override > 0)    settings.instrument_id     = instrument_id_override;

    ygg::logging::init("bridge", settings.logging);
    ygg::log::info("bridge starting — ws :{}  aeron {}", settings.ws_port, settings.media_driver_dir);
    ygg::log::info("[bridge] md_data stream={}  exec_report stream={}  control stream={}",
                   settings.md_data.stream_id, settings.exec_report.stream_id,
                   settings.control_command.stream_id);
    ygg::log::info("[bridge] mode={} strategy={} symbol={}@{} starting_capital=${:.2f} instrument_filter={}",
                   settings.mode, settings.strategy, settings.symbol, settings.exchange,
                   settings.starting_capital,
                   settings.instrument_id == 0 ? "(none)" : std::to_string(settings.instrument_id));

    // ── Aeron ────────────────────────────────────────────────────────────────
    auto aeron = ygg::aeron::connect(settings.media_driver_dir);

    bridge::MdSubscriber   md_sub(aeron, settings.md_data.channel, settings.md_data.stream_id);
    bridge::ExecSubscriber exec_sub(aeron, settings.exec_report.channel, settings.exec_report.stream_id);

    // ── Portfolio snapshot subscription (Fenrir → bridge) ────────────────────
    // Fenrir publishes JSON at ~10Hz with option legs, Greeks, and vol surface.
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

    // ── Control publication (bridge → Fenrir) ────────────────────────────────
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

        // Publish to Fenrir via Aeron
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
                                       settings.starting_capital));
    ws.publish(bridge::MsgKind::Status, bridge::encode::status("live"));

    // ── Position state (bridge is authoritative) ─────────────────────────────
    bridge::PositionTracker tracker(settings.starting_capital);
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
                                        res.equity));

        const double unreal = last_mid > 0 ? tracker.unrealized_pnl(last_mid) : 0.0;
        ws.publish(bridge::MsgKind::Position,
                   bridge::encode::position(settings.symbol, res.net_qty, res.avg_entry, unreal));
    });

    // ── Poll loop ────────────────────────────────────────────────────────────
    while (ygg::signal::is_running()) {
        int work = 0;
        work += md_sub.poll(32);
        work += exec_sub.poll(32);

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

        if (work == 0) std::this_thread::sleep_for(std::chrono::microseconds(500));
    }

    ygg::log::info("bridge shutting down");
    ws.publish(bridge::MsgKind::Status, bridge::encode::status("off"));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ws.stop();
    return 0;
}
