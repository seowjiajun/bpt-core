#include "backtester/clock/clock_master.h"

#include "backtester/data/orderbook_record.h"
#include "backtester/data/trade_record.h"
#include "backtester/exchange/okx_md_server.h"
#include "backtester/results/results_collector.h"

#include <messages/BacktestCommand.h>

#include <format>
#include <stdexcept>

namespace bpt::backtester::clock {

ClockMaster::ClockMaster(data::DataLoader& loader,
                         exchange::BinanceMdServer* binance_server,
                         exchange::OkxMdServer* okx_server,
                         matching::MatchingEngine* matching_engine,
                         results::ResultsCollector* results,
                         messaging::BacktestControlPublisher* ctrl_pub,
                         messaging::BacktestAckSubscriber* ack_sub)
    : loader_(loader),
      binance_server_(binance_server),
      okx_server_(okx_server),
      matching_engine_(matching_engine),
      results_(results),
      ctrl_pub_(ctrl_pub),
      ack_sub_(ack_sub) {}

void ClockMaster::run() {
    using bpt::messages::BacktestCommand;

    // Handshake: confirm Strategy is up and ready before releasing any ticks.
    if (ctrl_pub_) {
        bpt::common::log::info("[ClockMaster] Tick-gating enabled — sending handshake to Strategy");
        ctrl_pub_->send(BacktestCommand::START, 0, 0);
        if (!ack_sub_->wait_for(0, kAckTimeout))
            throw std::runtime_error("[ClockMaster] Handshake ack timed out — is Strategy running?");
        bpt::common::log::info("[ClockMaster] Handshake ack received, starting tick loop");
    }

    uint64_t seq = 0;

    while (auto event = loader_.next()) {
        dispatch(*event);
        ++seq;

        if (ctrl_pub_) {
            ctrl_pub_->send(BacktestCommand::START, seq, event->timestamp_ns);
            if (!ack_sub_->wait_for(seq, kAckTimeout))
                throw std::runtime_error(
                    std::format("[ClockMaster] Ack timed out at seq={} ts={}", seq, event->timestamp_ns));
        }

        if (seq % 100'000 == 0)
            bpt::common::log::debug("[ClockMaster] {} ticks processed, last_ts={}", seq, event->timestamp_ns);
    }

    if (ctrl_pub_) {
        ctrl_pub_->send(BacktestCommand::STOP, seq, 0);
        bpt::common::log::info("[ClockMaster] Sent STOP to Strategy");
    }

    bpt::common::log::info("[ClockMaster] Data exhausted after {} ticks.", seq);
}

void ClockMaster::dispatch(const data::MarketEvent& event) {
    const std::string& exchange = (event.type == data::MarketEvent::Type::TRADE)
                                      ? std::get<data::TradeRecord>(event.payload).exchange
                                      : std::get<data::OrderBookRecord>(event.payload).exchange;

    if (exchange == "BINANCE") {
        if (binance_server_)
            binance_server_->push(event);
    } else if (exchange == "OKX") {
        if (okx_server_)
            okx_server_->push(event);
    } else {
        bpt::common::log::warn("[ClockMaster] No WS server for exchange '{}' — event dropped", exchange);
    }

    if (matching_engine_)
        matching_engine_->on_market_event(event);
    if (results_)
        results_->on_market_event(event);
}

}  // namespace bpt::backtester::clock
