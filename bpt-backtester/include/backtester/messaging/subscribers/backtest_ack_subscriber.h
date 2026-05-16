#pragma once

/// \file
/// \brief Aeron subscriber for BacktestAck messages from Strategy.

#include <Aeron.h>

#include <chrono>
#include <cstdint>
#include <memory>

namespace bpt::backtester::messaging {

/// \brief Polls BacktestAck (SBE id=24) messages from Aeron stream 9001.
class BacktestAckSubscriber {
public:
    explicit BacktestAckSubscriber(std::shared_ptr<aeron::Subscription> sub);

    /// \brief Blocks until a BacktestAck with the given tick_seq arrives or the
    ///        timeout elapses.
    /// \param expected_seq Tick sequence number to wait for.
    /// \param timeout      Maximum time to wait.
    /// \return True on success, false on timeout.
    bool wait_for(uint64_t expected_seq, std::chrono::milliseconds timeout);

private:
    std::shared_ptr<aeron::Subscription> sub_;
};

}  // namespace bpt::backtester::messaging
