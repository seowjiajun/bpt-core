#include "strategy/unwind/graceful_unwinder.h"

#include "strategy/clock/sim_clock.h"

#include <messages/ExecStatus.h>
#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <messages/TimeInForce.h>

#include <bpt_common/logging.h>
#include <cmath>

namespace {
quill::Logger* kLog() {
    static quill::Logger* l = bpt::common::logging::get_logger("GracefulUnwinder");
    return l;
}
inline uint64_t now_ns() {
    return bpt::strategy::clock::SimClock::now_ns();
}
}  // namespace

namespace bpt::strategy::unwind {

using bpt::messages::ExecStatus;
using bpt::messages::OrderSide;
using bpt::messages::OrderType;
using bpt::messages::TimeInForce;

GracefulUnwinder::GracefulUnwinder(strategy::PositionTracker& positions, order::OrderManager& order_mgr, Config cfg)
    : positions_(positions),
      order_mgr_(order_mgr),
      cfg_(cfg) {}

void GracefulUnwinder::arm(std::vector<Instrument> instruments) {
    records_.clear();
    const uint64_t t = now_ns();

    for (auto& inst : instruments) {
        const auto [net_qty, abs_qty] = [&] {
            const double n = static_cast<double>(positions_.net_qty(inst.instrument_id, inst.exchange_id)) / 1e8;
            return std::pair{n, std::abs(n)};
        }();
        const double min_lot = (inst.lot_size > 0.0) ? inst.lot_size : 1e-8;

        if (abs_qty < min_lot) {
            bpt::common::log::info(kLog(), "{} already flat — skipping", inst.symbol);
            continue;
        }

        if (inst.price_ref <= 0.0) {
            bpt::common::log::warn(kLog(), "{} no price ref — starting in IOC phase", inst.symbol);
            Record r;
            r.instrument_id = inst.instrument_id;
            r.exchange_id = inst.exchange_id;
            r.tick_size = inst.tick_size;
            r.lot_size = inst.lot_size;
            r.symbol = std::move(inst.symbol);
            r.phase = Phase::kIoc;
            r.start_ns = t;
            r.last_step_ns = t;
            r.unwind_price = 0.0;
            r.retries_left = cfg_.max_retries;
            records_.push_back(std::move(r));
            continue;
        }

        const auto side = (net_qty > 0.0) ? OrderSide::SELL : OrderSide::BUY;

        Record r;
        r.instrument_id = inst.instrument_id;
        r.exchange_id = inst.exchange_id;
        r.tick_size = inst.tick_size;
        r.lot_size = inst.lot_size;
        r.symbol = std::move(inst.symbol);
        r.phase = Phase::kPassive;
        r.start_ns = t;
        r.last_step_ns = t;
        r.unwind_price = inst.price_ref;
        r.retries_left = cfg_.max_retries;

        bpt::common::log::info(kLog(),
                               "{} passive unwind at {:.6f} net_qty={:.4f} (timeout={:.0f}s)",
                               r.symbol,
                               r.unwind_price,
                               net_qty,
                               cfg_.passive_timeout_s);
        send_passive(r, side, abs_qty);
        records_.push_back(std::move(r));
    }
}

void GracefulUnwinder::tick() {
    const uint64_t t = now_ns();
    const uint64_t step_ns = static_cast<uint64_t>(cfg_.step_interval_s * 1e9);
    const uint64_t timeout_ns = static_cast<uint64_t>(cfg_.passive_timeout_s * 1e9);

    for (auto& r : records_) {
        if (r.phase != Phase::kPassive)
            continue;

        const auto [net_qty, abs_qty] = net_and_abs(r);
        const double min_lot = (r.lot_size > 0.0) ? r.lot_size : 1e-8;
        if (abs_qty < min_lot) {
            r.phase = Phase::kDone;
            continue;
        }
        const auto side = (net_qty > 0.0) ? OrderSide::SELL : OrderSide::BUY;

        if (t - r.start_ns >= timeout_ns) {
            bpt::common::log::info(kLog(), "{} passive timeout — escalating to IOC", r.symbol);
            r.phase = Phase::kIoc;
            if (r.h_unwind.live()) {
                order_mgr_.send_cancel(r.h_unwind);
                // on_exec_report(CANCELLED) fires advance_ioc()
            } else {
                advance_ioc(r);
            }
        } else if (t - r.last_step_ns >= step_ns) {
            const double tick = (r.tick_size > 0.0) ? r.tick_size : 1e-8;
            r.unwind_price += (side == OrderSide::BUY) ? tick : -tick;
            r.last_step_ns = t;
            bpt::common::log::info(kLog(), "{} step → {:.6f}", r.symbol, r.unwind_price);
            if (r.h_unwind.live()) {
                order_mgr_.send_cancel(r.h_unwind);
                // on_exec_report(CANCELLED) reposts at updated unwind_price
            } else {
                send_passive(r, side, abs_qty);
            }
        }
    }
}

void GracefulUnwinder::on_exec_report(const bpt::messages::ExecutionReport& rpt) {
    const auto status = rpt.status();
    const bool is_terminal =
        (status == ExecStatus::FILLED || status == ExecStatus::CANCELLED || status == ExecStatus::REJECTED);
    if (!is_terminal)
        return;

    const uint64_t instrument_id = rpt.instrumentId();
    for (auto& r : records_) {
        if (r.instrument_id != instrument_id)
            continue;
        if (!r.h_unwind.valid() || r.h_unwind.order_id() != rpt.orderId())
            continue;

        r.h_unwind.reset();

        const auto [net_qty, abs_qty] = net_and_abs(r);
        const double min_lot = (r.lot_size > 0.0) ? r.lot_size : 1e-8;

        if (abs_qty < min_lot) {
            r.phase = Phase::kDone;
            bpt::common::log::info(kLog(), "{} flat", r.symbol);
            return;
        }

        const auto side = (net_qty > 0.0) ? OrderSide::SELL : OrderSide::BUY;

        if (r.phase == Phase::kPassive) {
            send_passive(r, side, abs_qty);
        } else if (r.phase == Phase::kIoc) {
            advance_ioc(r);
        }
        return;
    }
}

bool GracefulUnwinder::pending() const {
    for (const auto& r : records_) {
        if (r.phase != Phase::kDone)
            return true;
    }
    return false;
}

double GracefulUnwinder::drain_budget_s() const {
    return cfg_.passive_timeout_s + static_cast<double>(cfg_.max_retries) * 5.0 + 15.0;
}

void GracefulUnwinder::send_passive(Record& r, OrderSide::Value side, double qty) {
    if (r.unwind_price <= 0.0)
        return;
    const double tick = (r.tick_size > 0.0) ? r.tick_size : 1e-8;
    const double price = std::round(r.unwind_price / tick) * tick;
    const order::NewOrderRequest req{
        .instrument_id = r.instrument_id,
        .exchange_id = r.exchange_id,
        .side = side,
        .type = OrderType::LIMIT,
        .tif = TimeInForce::GTC,
        .price = price,
        .qty = qty,
        .exec_inst = {},
    };
    r.h_unwind = order_mgr_.send_new_order(req, kTag);
    if (r.h_unwind.valid())
        bpt::common::log::info(kLog(),
                               "{} PASSIVE {} {:.6f} qty={:.4f} oid={}",
                               r.symbol,
                               (side == OrderSide::BUY) ? "BUY" : "SELL",
                               price,
                               qty,
                               r.h_unwind.order_id());
}

void GracefulUnwinder::send_ioc(Record& r, OrderSide::Value side, double qty, double cross_bps) {
    const double factor = (side == OrderSide::BUY) ? (1.0 + cross_bps / 10000.0) : (1.0 - cross_bps / 10000.0);
    const double price = r.unwind_price * factor;
    if (price <= 0.0)
        return;
    const order::NewOrderRequest req{
        .instrument_id = r.instrument_id,
        .exchange_id = r.exchange_id,
        .side = side,
        .type = OrderType::LIMIT,
        .tif = TimeInForce::IOC,
        .price = price,
        .qty = qty,
        .exec_inst = {},
    };
    r.h_unwind = order_mgr_.send_new_order(req, kTag);
    if (r.h_unwind.valid())
        bpt::common::log::info(kLog(),
                               "{} IOC {} {:.6f} qty={:.4f} cross={:.0f}bps oid={}",
                               r.symbol,
                               (side == OrderSide::BUY) ? "BUY" : "SELL",
                               price,
                               qty,
                               cross_bps,
                               r.h_unwind.order_id());
}

void GracefulUnwinder::advance_ioc(Record& r) {
    const auto [net_qty, abs_qty] = net_and_abs(r);
    const double min_lot = (r.lot_size > 0.0) ? r.lot_size : 1e-8;
    if (abs_qty < min_lot) {
        r.phase = Phase::kDone;
        return;
    }
    if (r.retries_left == 0) {
        bpt::common::log::error(kLog(), "{} IOC retries exhausted, net_qty={:.4f} unflat", r.symbol, net_qty);
        r.phase = Phase::kDone;
        return;
    }
    --r.retries_left;
    ++r.attempt;
    const auto side = (net_qty > 0.0) ? OrderSide::SELL : OrderSide::BUY;
    const double cross = cfg_.cross_bps + static_cast<double>(r.attempt) * 10.0;
    bpt::common::log::info(kLog(),
                           "{} IOC retry ({} left) cross={:.0f}bps net_qty={:.4f}",
                           r.symbol,
                           r.retries_left,
                           cross,
                           net_qty);
    send_ioc(r, side, abs_qty, cross);
}

std::pair<double, double> GracefulUnwinder::net_and_abs(const Record& r) const {
    const double net = static_cast<double>(positions_.net_qty(r.instrument_id, r.exchange_id)) / 1e8;
    return {net, std::abs(net)};
}

}  // namespace bpt::strategy::unwind
