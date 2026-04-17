#pragma once

#include <Aeron.h>

#include <messages/BacktestCommand.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace bpt::strategy::backtest {

// Subscribes to BacktestControl (stream 9002, Backtester → Strategy) and publishes
// BacktestAck (stream 9001, Strategy → Backtester).
//
// Only instantiated when backtest_mode = true in the config.
class BacktestClient {
public:
    BacktestClient(std::shared_ptr<aeron::Aeron> aeron,
                   const std::string& channel,
                   int32_t control_stream_id,  // subscribe to BacktestControl
                   int32_t ack_stream_id,      // publish  to BacktestAck
                   int pub_timeout_ms,
                   int pub_poll_interval_ms);

    // Fired for each BacktestControl fragment decoded.
    std::function<void(bpt::messages::BacktestCommand::Value cmd, uint64_t tick_seq, uint64_t simulation_ts)>
        on_control;

    // Poll the control subscription; returns fragment count.
    int poll();

    // Encode and offer a BacktestAck on the ack publication.
    void send_ack(uint64_t tick_seq, uint64_t simulation_ts);

private:
    std::shared_ptr<aeron::Subscription> ctrl_sub_;
    std::shared_ptr<aeron::Publication> ack_pub_;

    static constexpr std::size_t kAckBufSize = 24;  // 8-byte header + 16-byte body
    char ack_buf_[kAckBufSize]{};
};

}  // namespace bpt::strategy::backtest
