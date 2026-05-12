#include "backtester/clock/clock_master.h"

#include "backtester/data/orderbook_record.h"
#include "backtester/data/trade_record.h"
#include "backtester/exchange/okx_md_server.h"
#include "backtester/results/results_collector.h"

#include <messages/BacktestCommand.h>
#include <messages/ExchangeRegistry.h>

#include <bpt_common/logging.h>
#include <format>
#include <stdexcept>

namespace bpt::backtester::clock {

namespace {
quill::Logger* kLog() {
    static quill::Logger* l = bpt::common::logging::get_logger("ClockMaster");
    return l;
}
}  // namespace

ClockMaster::ClockMaster(data::DataLoader& loader,
                         exchange::BinanceMdServer* binance_server,
                         exchange::OkxMdServer* okx_server,
                         exchange::HyperliquidMdServer* hyperliquid_server,
                         matching::MatchingEngine* matching_engine,
                         results::ResultsCollector* results,
                         messaging::BacktestControlPublisher* ctrl_pub,
                         messaging::BacktestAckSubscriber* ack_sub)
    : loader_(loader),
      binance_server_(binance_server),
      okx_server_(okx_server),
      hyperliquid_server_(hyperliquid_server),
      matching_engine_(matching_engine),
      results_(results),
      ctrl_pub_(ctrl_pub),
      ack_sub_(ack_sub) {}

void ClockMaster::run() {
    using bpt::messages::BacktestCommand;

    // Handshake: confirm Strategy is up and ready before releasing any ticks.
    if (ctrl_pub_) {
        bpt::common::log::info(kLog(), "Tick-gating enabled — sending handshake to Strategy");
        ctrl_pub_->send(BacktestCommand::START, 0, 0);
        if (!ack_sub_->wait_for(0, kAckTimeout))
            throw std::runtime_error("Handshake ack timed out — is Strategy running?");
        bpt::common::log::info(kLog(), "Handshake ack received, starting tick loop");
    }

    uint64_t seq = 0;

    while (auto event = loader_.next()) {
        dispatch(*event);
        ++seq;

        if (ctrl_pub_) {
            ctrl_pub_->send(BacktestCommand::START, seq, event->timestamp_ns);
            if (!ack_sub_->wait_for(seq, kAckTimeout))
                throw std::runtime_error(std::format("Ack timed out at seq={} ts={}", seq, event->timestamp_ns));
        }

        if (seq % 100'000 == 0)
            bpt::common::log::debug(kLog(), "{} ticks processed, last_ts={}", seq, event->timestamp_ns);
    }

    if (ctrl_pub_) {
        ctrl_pub_->send(BacktestCommand::STOP, seq, 0);
        bpt::common::log::info(kLog(), "Sent STOP to Strategy");
    }

    bpt::common::log::info(kLog(), "Data exhausted after {} ticks.", seq);
}

void ClockMaster::dispatch(const data::MarketEvent& event) {
    const std::string& exchange = (event.type == data::MarketEvent::Type::TRADE)
                                      ? std::get<data::TradeRecord>(event.payload).exchange
                                      : std::get<data::OrderBookRecord>(event.payload).exchange;

    // Hot dispatch path. ExchangeRegistry::from_name iterates a 4-element
    // constexpr table — same order-of-magnitude as the previous string
    // chain but routed through canonical IDs so a typo in source data
    // becomes a single warning rather than a silent skip.
    const auto exch_id = bpt::messages::ExchangeRegistry::from_name(exchange);
    if (!exch_id) {
        bpt::common::log::warn(kLog(), "Unknown exchange '{}' in market event — dropped", exchange);
    } else {
        switch (*exch_id) {
            case bpt::messages::ExchangeId::BINANCE:
                if (binance_server_)
                    binance_server_->push(event);
                break;
            case bpt::messages::ExchangeId::OKX:
                if (okx_server_)
                    okx_server_->push(event);
                break;
            case bpt::messages::ExchangeId::HYPERLIQUID:
                if (hyperliquid_server_)
                    hyperliquid_server_->push(event);
                break;
            default:
                bpt::common::log::warn(kLog(), "No WS server for exchange '{}' — event dropped", exchange);
                break;
        }
    }

    if (matching_engine_)
        matching_engine_->on_market_event(event);
    if (results_)
        results_->on_market_event(event);
}

}  // namespace bpt::backtester::clock
