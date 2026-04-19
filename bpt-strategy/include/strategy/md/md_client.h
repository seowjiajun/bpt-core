#pragma once

#include <Aeron.h>
#include <FragmentAssembler.h>

#include <messages/AckStatus.h>
#include <messages/MdMarketData.h>
#include <messages/MdOrderBook.h>
#include <messages/MdTrade.h>
#include <messages/TradeSide.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bpt::strategy::md {

// Client for the market-data service.
//
// Publishes MdSubscribeBatch on stream 2001, subscribes to MdMarketData +
// MdTrade on stream 2002, and subscribes to acks + heartbeats on stream 2003.
//
// Single-threaded: only call from the strategy poll thread.
class MdClient {
public:
    struct InstrumentDesc {
        uint64_t instrument_id;
        std::string exchange;  // e.g. "BINANCE"
        std::string symbol;    // exchange-native symbol, e.g. "BTCUSDT"
        uint8_t depth{0};      // 0 = BBO only, 5 = top-5 order book levels
    };

    using OnBboFn = std::function<void(const bpt::messages::MdMarketData&)>;
    using OnTradeFn = std::function<void(const bpt::messages::MdTrade&)>;
    using OnOrderBookFn = std::function<void(const bpt::messages::MdOrderBook&)>;
    using OnServiceHeartbeatFn = std::function<void()>;

    MdClient(std::shared_ptr<aeron::Aeron> aeron,
             const std::string& channel,
             int control_stream,
             int data_stream,
             int ack_hb_stream);

    // Send a full-replace subscription batch.
    void subscribe(uint64_t correlation_id, const std::vector<InstrumentDesc>& instruments);

    // Poll both data and ack/hb streams.  Returns total fragment count.
    int poll(int fragment_limit = 10);

    // Fired for each BBO tick received from the MD service.
    // Mutually exclusive with on_order_book — MdGateway publishes one or the other
    // depending on its order_book_depth config (0 = BBO, N > 0 = order book).
    OnBboFn on_bbo;

    // Fired for each trade tick received from the MD service.
    OnTradeFn on_trade;

    // Fired for each order book snapshot received from the MD service.
    // Only populated when MdGateway is configured with order_book_depth > 0.
    OnOrderBookFn on_order_book;

    // Fired each time a MdServiceHeartbeat is received from MdGateway.
    // Used by StrategyApp to track local receipt time for the liveness watchdog.
    OnServiceHeartbeatFn on_service_heartbeat;

    // Nanosecond timestamp of the last MdServiceHeartbeat (0 if none yet).
    [[nodiscard]] uint64_t last_service_heartbeat_ns() const { return last_service_hb_ns_; }

private:
    void handle_data_fragment(aeron::AtomicBuffer& buffer,
                              aeron::util::index_t offset,
                              aeron::util::index_t length,
                              aeron::Header& header);

    void handle_ack_hb_fragment(aeron::AtomicBuffer& buffer,
                                aeron::util::index_t offset,
                                aeron::util::index_t length,
                                aeron::Header& header);

    std::shared_ptr<aeron::Publication> ctrl_pub_;
    std::shared_ptr<aeron::Subscription> data_sub_;
    std::shared_ptr<aeron::Subscription> ack_hb_sub_;
    std::unique_ptr<aeron::FragmentAssembler> data_assembler_;
    uint64_t last_service_hb_ns_{0};
};

}  // namespace bpt::strategy::md
