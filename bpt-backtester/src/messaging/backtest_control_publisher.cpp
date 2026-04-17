#include "backtester/messaging/backtest_control_publisher.h"

#include <concurrent/AtomicBuffer.h>

#include <messages/BacktestControl.h>
#include <messages/MessageHeader.h>

#include <chrono>
#include <stdexcept>
#include <thread>

namespace bpt::backtester::messaging {

using namespace bpt::messages;
using namespace std::chrono_literals;

BacktestControlPublisher::BacktestControlPublisher(std::shared_ptr<aeron::Publication> pub) : pub_(std::move(pub)) {}

void BacktestControlPublisher::send(BacktestCommand::Value cmd, uint64_t tick_seq, uint64_t simulation_ts) {
    BacktestControl ctrl;
    ctrl.wrapAndApplyHeader(buf_, 0, kBufSize).command(cmd).tickSeqNum(tick_seq).simulationTs(simulation_ts);

    aeron::concurrent::AtomicBuffer ab(reinterpret_cast<uint8_t*>(buf_), kBufSize);

    // Spin on back-pressure for up to 30 seconds.
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (true) {
        int64_t result = pub_->offer(ab, 0, static_cast<aeron::index_t>(kBufSize));
        if (result > 0)
            return;

        if (result == aeron::BACK_PRESSURED || result == aeron::NOT_CONNECTED || result == aeron::ADMIN_ACTION) {
            if (std::chrono::steady_clock::now() > deadline)
                throw std::runtime_error("[BacktestControlPublisher] offer timed out (back-pressure/not-connected)");
            std::this_thread::sleep_for(100us);
            continue;
        }

        throw std::runtime_error("[BacktestControlPublisher] offer failed: " + std::to_string(result));
    }
}

}  // namespace bpt::backtester::messaging
