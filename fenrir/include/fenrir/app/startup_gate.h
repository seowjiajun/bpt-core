#pragma once

#include "fenrir/metrics/metrics.h"
#include "fenrir/order/order_gateway_client.h"
#include "fenrir/refdata/refdata_client.h"
#include "fenrir/strategy/i_strategy.h"

#include <bifrost_protocol/ExchangeId.h>

#include <cstdint>

namespace fenrir::app {

// Drives Fenrir's startup sequence as a small explicit state machine.
//
// The original ad-hoc gate (refdata_ready_ / account_snapshot_requests_sent_
// / account_snapshot_ready_ / strategy_started_ / strategy_md_started_)
// sprawled across FenrirApp's run loop and several callbacks, which is the
// kind of layout where bugs hide — e.g. the AccountSnapshot-only-fires-once
// class of issues we just chased through the dashboard.
//
// Phases:
//   WaitRefdata          — waiting for RefdataClient::on_ready
//   WaitAccountSnapshots — refdata ready; AccountSnapshotRequests pending or in-flight
//   WaitMdSnapshot       — accounts ready; refdata->subscribe() pending or in-flight
//   Active               — strategy is fully running on live MD
//
// State transitions:
//   - Setters (on_refdata_ready / on_account_snapshot /
//     on_refdata_snapshot_complete) only mutate internal state. They are
//     safe to call from inside another client's poll handler.
//   - tick() performs all deferred side effects (sending requests, calling
//     subscribe). Call once per main-loop iteration.
class StartupGate {
public:
    enum class Phase {
        WaitRefdata,
        WaitAccountSnapshots,
        WaitMdSnapshot,
        Active,
    };

    // order_gw may be null in refdata-only / dry-run modes; in that case the
    // account-snapshot phase is skipped entirely.
    StartupGate(refdata::RefdataClient& refdata,
                order::OrderGatewayClient* order_gw,
                strategy::IStrategy& strategy,
                metrics::FenrirMetrics& metrics,
                uint8_t configured_exchanges_mask,
                uint64_t correlation_id);

    // ── Inputs ────────────────────────────────────────────────────────────
    //
    // Called from RefdataClient::on_ready. Validates that all required
    // exchanges are loaded; on missing exchanges, logs critical and stops
    // the process (refuses to trade with partial refdata).
    void on_refdata_ready(uint8_t exchanges_loaded,
                          uint16_t instrument_count,
                          bool fee_schedules_loaded,
                          bool funding_rates_loaded);

    // Called from OrderGatewayClient::on_account_snapshot for each exchange.
    void on_account_snapshot(bifrost::protocol::ExchangeId::Value exchange);

    // Called from RefdataClient::on_snapshot_complete after the strategy
    // has consumed the snapshot itself.
    void on_refdata_snapshot_complete();

    // Called once per main-loop iteration. Performs deferred side effects
    // and advances the state machine.
    void tick();

    Phase phase() const noexcept { return phase_; }
    bool is_active() const noexcept { return phase_ == Phase::Active; }

private:
    void send_account_snapshot_requests();

    refdata::RefdataClient& refdata_;
    order::OrderGatewayClient* order_gw_;  // nullable
    strategy::IStrategy& strategy_;
    metrics::FenrirMetrics& metrics_;

    const uint8_t configured_mask_;
    uint64_t correlation_id_;

    Phase phase_{Phase::WaitRefdata};
    bool refdata_ready_{false};
    uint8_t accounts_ready_mask_{0};
    bool account_requests_sent_{false};
    bool md_subscribe_sent_{false};
};

}  // namespace fenrir::app
