#pragma once

#include <Aeron.h>

#include <chrono>
#include <cstdint>
#include <memory>

namespace bpt::backtester::messaging {

// Polls BacktestAck (SBE id=24) messages from Aeron stream 9001.
class BacktestAckSubscriber {
public:
    explicit BacktestAckSubscriber(std::shared_ptr<aeron::Subscription> sub);

    // Blocks until a BacktestAck with the given tick_seq arrives or the
    // timeout elapses.  Returns true on success, false on timeout.
    bool wait_for(uint64_t expected_seq, std::chrono::milliseconds timeout);

private:
    std::shared_ptr<aeron::Subscription> sub_;
};

}  // namespace bpt::backtester::messaging
