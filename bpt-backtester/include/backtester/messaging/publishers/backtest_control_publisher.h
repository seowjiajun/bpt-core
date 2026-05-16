#pragma once

/// \file
/// \brief Aeron publisher for BacktestControl messages to Strategy.

#include <Aeron.h>

#include <messages/BacktestCommand.h>

#include <cstdint>
#include <memory>

namespace bpt::backtester::messaging {

/// \brief Encodes and publishes BacktestControl (SBE id=25) messages on Aeron stream 9002.
class BacktestControlPublisher {
public:
    explicit BacktestControlPublisher(std::shared_ptr<aeron::Publication> pub);

    /// \brief Encodes a BacktestControl message and offers it on the publication.
    ///
    /// Spins on back-pressure for up to ~1 s then throws.
    /// \param cmd          Backtest command discriminator.
    /// \param tick_seq     Monotonic tick sequence.
    /// \param simulation_ts Simulated timestamp for the tick.
    void send(bpt::messages::BacktestCommand::Value cmd, uint64_t tick_seq, uint64_t simulation_ts);

private:
    std::shared_ptr<aeron::Publication> pub_;

    // MessageHeader (8) + BacktestControl block (17)
    static constexpr std::size_t kBufSize = 25;
    char buf_[kBufSize]{};
};

}  // namespace bpt::backtester::messaging
