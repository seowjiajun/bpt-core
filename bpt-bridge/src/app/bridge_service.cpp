#include "bridge/app/bridge_service.h"

#include "bridge/aeron/account_subscriber.h"
#include "bridge/aeron/exec_subscriber.h"
#include "bridge/aeron/md_subscriber.h"
#include "bridge/ws/message_encoder.h"
#include "bridge/state/position_tracker.h"
#include "bridge/ws/ws_server.h"

#include <analytics/messaging/toxicity_update.h>
#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>
#include <bpt_common/signal.h>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace bpt::bridge {

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

    MdSubscriber md_sub(aeron_, settings_.md_data.channel, settings_.md_data.stream_id);
    ExecSubscriber exec_sub(aeron_, settings_.exec_report.channel, settings_.exec_report.stream_id);
    AccountSubscriber account_sub(aeron_, settings_.account_snapshot.channel, settings_.account_snapshot.stream_id);

    std::shared_ptr<aeron::Subscription> snapshot_sub;
    if (settings_.portfolio.stream_id != 0) {
        snapshot_sub = bpt::common::aeron::wait_for_subscription(aeron_,
                                                                 settings_.portfolio.channel,
                                                                 settings_.portfolio.stream_id);
        bpt::common::log::info("portfolio snapshot subscription ready on stream {}", settings_.portfolio.stream_id);
    }

    std::shared_ptr<aeron::Subscription> tox_sub;
    if (settings_.toxicity.stream_id != 0) {
        tox_sub =
            bpt::common::aeron::wait_for_subscription(aeron_, settings_.toxicity.channel, settings_.toxicity.stream_id);
        bpt::common::log::info("toxicity subscription ready on stream {}", settings_.toxicity.stream_id);
    }

    // Control publication (bridge → Strategy): 1-byte commands 0x00=HALT, 0x01=RESUME.
    std::shared_ptr<aeron::Publication> ctrl_pub;
    if (settings_.dashboard_control.stream_id != 0) {
        ctrl_pub = bpt::common::aeron::wait_for_publication(aeron_,
                                                            settings_.dashboard_control.channel,
                                                            settings_.dashboard_control.stream_id);
        bpt::common::log::info("control publication ready on stream {}", settings_.dashboard_control.stream_id);
    }

    WsServer ws(settings_.ws_port);

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
            bpt::common::log::warn("unknown command: {}", cmd);
            return;
        }

        if (ctrl_pub) {
            aeron::AtomicBuffer buf(reinterpret_cast<uint8_t*>(&ctrl_byte), 1);
            auto result = ctrl_pub->offer(buf, 0, 1);
            if (result < 0) {
                bpt::common::log::warn("control offer failed: {}", result);
            }
        }

        ws.publish(MsgKind::Status, encode::status(status_str));
        bpt::common::log::info("command '{}' → status '{}'", cmd, status_str);
    };

    ws.start();

    ws.publish(MsgKind::Session,
               encode::session(settings_.symbol,
                               settings_.strategy,
                               settings_.exchange,
                               settings_.mode,
                               settings_.instrument_type));
    ws.publish(MsgKind::Status, encode::status("live"));

    PositionTracker tracker;
    double last_mid = 0.0;

    // Tick throttle — BBO mids update ~1000 Hz but the dashboard only
    // needs ~30 Hz for a smooth visual. Most-recent value wins.
    using clock = std::chrono::steady_clock;
    constexpr auto kTickMinInterval = std::chrono::milliseconds(33);
    auto last_tick_bcast = clock::now() - std::chrono::seconds(1);

    md_sub.set_handler([&](uint64_t instr, double mid, uint64_t ts_ns) {
        if (settings_.instrument_id != 0 && instr != settings_.instrument_id)
            return;
        last_mid = mid;
        const auto now = clock::now();
        if (now - last_tick_bcast < kTickMinInterval)
            return;
        last_tick_bcast = now;
        ws.publish(MsgKind::Tick, encode::tick(ts_ns, settings_.symbol, mid));
    });

    exec_sub.set_order_handler([&](const ExecSubscriber::OrderEvent& ev) {
        if (settings_.instrument_id != 0 && ev.instrument_id != settings_.instrument_id)
            return;

        static const char* kStatusStr[] = {"acked", "filled", "partial", "rejected", "cancelled"};
        static const char* kTypeStr[] = {"MARKET", "LIMIT"};

        const char* status_s = ev.status < 5 ? kStatusStr[ev.status] : "unknown";
        const char* type_s = ev.order_type < 2 ? kTypeStr[ev.order_type] : "UNKNOWN";

        ws.publish(MsgKind::Order,
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
    });

    account_sub.set_handler([&](const AccountSubscriber::Snapshot& s) {
        std::vector<encode::AccountPosition> positions;
        positions.reserve(s.positions.size());
        for (const auto& p : s.positions) {
            positions.push_back({p.exchange_symbol, p.net_qty, p.avg_entry, p.unrealized_pnl});
        }
        std::vector<encode::AccountCurrencyBalance> ccy_balances;
        ccy_balances.reserve(s.currency_balances.size());
        for (const auto& cb : s.currency_balances) {
            ccy_balances.push_back({cb.ccy, cb.equity, cb.available_balance});
        }
        ws.publish(MsgKind::Order,
                   encode::account(s.ts_ns, s.available_balance, s.total_equity, positions, ccy_balances));
    });

    exec_sub.set_handler([&](const ExecSubscriber::Fill& f) {
        if (settings_.instrument_id != 0 && f.instrument_id != settings_.instrument_id)
            return;

        const auto res = tracker.apply(f.side, f.qty, f.price);

        static const char* kTypeStr[] = {"MARKET", "LIMIT"};
        const char* type_s = f.order_type < 2 ? kTypeStr[f.order_type] : "UNKNOWN";
        ws.publish(MsgKind::Fill,
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

        const double unreal = last_mid > 0 ? tracker.unrealized_pnl(last_mid) : 0.0;
        ws.publish(MsgKind::Position, encode::position(settings_.symbol, res.net_qty, res.avg_entry, unreal));
    });

    // Poll loop — driven by bpt::common::signal::is_running() since
    // bpt::app::run() installed the signal handler before us.
    while (bpt::common::signal::is_running()) {
        int work = 0;
        work += md_sub.poll(32);
        work += exec_sub.poll(32);
        work += account_sub.poll(8);

        if (snapshot_sub) {
            work += snapshot_sub->poll(
                [&ws](aeron::AtomicBuffer& buffer,
                      aeron::util::index_t offset,
                      aeron::util::index_t length,
                      aeron::Header& /*hdr*/) {
                    std::string json(reinterpret_cast<const char*>(buffer.buffer() + offset),
                                     static_cast<std::size_t>(length));
                    ws.publish(MsgKind::Order, std::move(json));
                },
                1);
        }

        if (tox_sub) {
            work += tox_sub->poll(
                [&ws](aeron::AtomicBuffer& buffer,
                      aeron::util::index_t offset,
                      aeron::util::index_t length,
                      aeron::Header& /*hdr*/) {
                    if (static_cast<std::size_t>(length) != sizeof(bpt::analytics::messaging::ToxicityUpdate))
                        return;
                    bpt::analytics::messaging::ToxicityUpdate u;
                    std::memcpy(&u, buffer.buffer() + offset, sizeof(u));
                    ws.publish(MsgKind::Toxicity,
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
                },
                4);
        }

        if (work == 0)
            std::this_thread::sleep_for(std::chrono::microseconds(500));
    }

    // Graceful shutdown on signal — notify dashboard that bridge is going
    // away, give WS clients a beat to flush, then stop the WS server.
    ws.publish(MsgKind::Status, encode::status("off"));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ws.stop();
}

}  // namespace bpt::bridge
