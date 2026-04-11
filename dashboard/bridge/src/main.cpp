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
    for (int i = 1; i < argc - 1; ++i) {
        if (std::string(argv[i]) == "--config") config_path = argv[i + 1];
    }

    bridge::config::Settings settings;
    try {
        settings = bridge::config::load(config_path);
    } catch (const std::exception& e) {
        ygg::logging::init("bridge");
        ygg::log::error("Failed to load config: {}", e.what());
        return 1;
    }

    ygg::logging::init("bridge", settings.logging);
    ygg::log::info("bridge starting — ws :{}  aeron {}", settings.ws_port, settings.media_driver_dir);
    ygg::log::info("[bridge] md_data stream={}  exec_report stream={}",
                   settings.md_data.stream_id, settings.exec_report.stream_id);
    ygg::log::info("[bridge] symbol={} starting_capital=${:.2f}",
                   settings.symbol, settings.starting_capital);

    // ── Aeron ────────────────────────────────────────────────────────────────
    auto aeron = ygg::aeron::connect(settings.media_driver_dir);

    bridge::MdSubscriber   md_sub(aeron, settings.md_data.channel, settings.md_data.stream_id);
    bridge::ExecSubscriber exec_sub(aeron, settings.exec_report.channel, settings.exec_report.stream_id);

    // ── WebSocket server ─────────────────────────────────────────────────────
    bridge::WsServer ws(settings.ws_port);
    ws.start();

    ws.publish(bridge::MsgKind::Session,
               bridge::encode::session(settings.symbol, settings.starting_capital));
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
    md_sub.set_handler([&](uint64_t /*instr*/, double mid, uint64_t ts_ns) {
        last_mid = mid;
        const auto now = clock::now();
        if (now - last_tick_bcast < kTickMinInterval) return;
        last_tick_bcast = now;
        ws.publish(bridge::MsgKind::Tick, bridge::encode::tick(ts_ns, settings.symbol, mid));
    });

    exec_sub.set_handler([&](const bridge::ExecSubscriber::Fill& f) {
        const auto res = tracker.apply(f.side, f.qty, f.price);

        ws.publish(bridge::MsgKind::Fill,
                   bridge::encode::fill(f.ts_ns,
                                        f.order_id,
                                        settings.symbol,
                                        f.side,
                                        f.qty,
                                        f.price,
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
        if (work == 0) std::this_thread::sleep_for(std::chrono::microseconds(500));
    }

    ygg::log::info("bridge shutting down");
    ws.publish(bridge::MsgKind::Status, bridge::encode::status("off"));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ws.stop();
    return 0;
}
