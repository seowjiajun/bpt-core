#include "strategy/app/startup_gate.h"

#include <bpt_common/logging.h>
#include <bpt_common/signal.h>

namespace bpt::strategy::app {

using bpt::messages::ExchangeId;

StartupGate::StartupGate(refdata::IRefdataClient& refdata,
                         order::IOrderGatewayClient* order_gw,
                         strategy::IStrategy& strategy,
                         metrics::StrategyMetrics& metrics,
                         uint8_t configured_exchanges_mask,
                         uint64_t correlation_id)
    : refdata_(refdata),
      order_gw_(order_gw),
      strategy_(strategy),
      metrics_(metrics),
      configured_mask_(configured_exchanges_mask),
      correlation_id_(correlation_id) {}

void StartupGate::on_refdata_ready(uint8_t exchanges_loaded,
                                   uint16_t instrument_count,
                                   bool fee_schedules_loaded,
                                   bool funding_rates_loaded) {
    if (!refdata_ready_) {
        bpt::common::log::info(
            "RefDataReady: exchanges=0x{:02x} (configured=0x{:02x}) "
            "instruments={} fee_schedules={} funding_rates={}",
            exchanges_loaded,
            configured_mask_,
            instrument_count,
            fee_schedules_loaded,
            funding_rates_loaded);
    } else {
        bpt::common::log::debug("RefDataReady (periodic): exchanges=0x{:02x} instruments={}",
                                exchanges_loaded,
                                instrument_count);
    }

    // Refuse to trade with partial refdata. This guard runs every time
    // on_ready fires so a mid-run loss of an exchange also halts.
    if (configured_mask_ != 0 && (exchanges_loaded & configured_mask_) != configured_mask_) {
        const uint8_t missing = configured_mask_ & ~exchanges_loaded;
        bpt::common::log::critical(
            "HALT — refdata service is missing required exchanges (mask=0x{:02x}). "
            "Cannot trade safely. Shutting down.",
            missing);
        bpt::common::signal::stop();
        return;
    }

    refdata_ready_ = true;
    if (exchanges_loaded & 0x01)
        metrics_.refdata_ready("BINANCE").Set(1.0);
    if (exchanges_loaded & 0x02)
        metrics_.refdata_ready("OKX").Set(1.0);
    if (exchanges_loaded & 0x04)
        metrics_.refdata_ready("HYPERLIQUID").Set(1.0);
    if (exchanges_loaded & 0x08)
        metrics_.refdata_ready("DERIBIT").Set(1.0);
}

void StartupGate::on_account_snapshot(ExchangeId::Value exchange) {
    uint8_t bit = 0;
    switch (exchange) {
        case ExchangeId::BINANCE:
            bit = 0x01;
            break;
        case ExchangeId::OKX:
            bit = 0x02;
            break;
        case ExchangeId::HYPERLIQUID:
            bit = 0x04;
            break;
        case ExchangeId::DERIBIT:
            bit = 0x08;
            break;
        default:
            break;
    }
    accounts_ready_mask_ |= bit;
}

void StartupGate::on_refdata_snapshot_complete() {
    if (phase_ != Phase::WaitMdSnapshot)
        return;

    bpt::common::log::info("Snapshot received — starting strategy MD subscriptions");
    strategy_.start();
    metrics_.strategy_active->Set(1.0);
    phase_ = Phase::Active;
}

void StartupGate::send_account_snapshot_requests() {
    if (!order_gw_)
        return;

    uint64_t corr = correlation_id_;
    if (configured_mask_ & 0x01) {
        bpt::common::log::info("Requesting AccountSnapshot for BINANCE");
        order_gw_->send_account_snapshot_request(ExchangeId::BINANCE, corr++);
    }
    if (configured_mask_ & 0x02) {
        bpt::common::log::info("Requesting AccountSnapshot for OKX");
        order_gw_->send_account_snapshot_request(ExchangeId::OKX, corr++);
    }
    if (configured_mask_ & 0x04) {
        bpt::common::log::info("Requesting AccountSnapshot for HYPERLIQUID");
        order_gw_->send_account_snapshot_request(ExchangeId::HYPERLIQUID, corr++);
    }
    if (configured_mask_ & 0x08) {
        bpt::common::log::info("Requesting AccountSnapshot for DERIBIT");
        order_gw_->send_account_snapshot_request(ExchangeId::DERIBIT, corr++);
    }
}

void StartupGate::tick() {
    switch (phase_) {
        case Phase::WaitRefdata:
            if (refdata_ready_) {
                if (order_gw_ && configured_mask_ != 0 && !account_requests_sent_) {
                    send_account_snapshot_requests();
                    account_requests_sent_ = true;
                }
                phase_ = Phase::WaitAccountSnapshots;
            }
            break;

        case Phase::WaitAccountSnapshots: {
            // If running without an order gateway, there are no accounts to wait for.
            const bool accounts_ready = !order_gw_ || (accounts_ready_mask_ & configured_mask_) == configured_mask_;
            if (!accounts_ready)
                break;

            if (!md_subscribe_sent_) {
                bpt::common::log::info("RefDataReady + AccountSnapshot received — sending RefDataSubscriptionRequest");
                refdata_.subscribe(correlation_id_);
                md_subscribe_sent_ = true;
            }
            phase_ = Phase::WaitMdSnapshot;
            break;
        }

        case Phase::WaitMdSnapshot:
            // Transition fires from on_refdata_snapshot_complete.
            break;

        case Phase::Active:
            break;
    }
}

}  // namespace bpt::strategy::app
