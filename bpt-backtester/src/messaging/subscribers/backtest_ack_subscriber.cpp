#include "backtester/messaging/subscribers/backtest_ack_subscriber.h"

#include <concurrent/AtomicBuffer.h>

#include <messages/BacktestAck.h>
#include <messages/MessageHeader.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace bpt::backtester::messaging {

using bpt::messages::BacktestAck;
using bpt::messages::MessageHeader;
using namespace std::chrono_literals;

BacktestAckSubscriber::BacktestAckSubscriber(std::shared_ptr<aeron::Subscription> sub) : sub_(std::move(sub)) {}

bool BacktestAckSubscriber::wait_for(uint64_t expected_seq, std::chrono::milliseconds timeout) {
    bool found = false;
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    auto handler = [&](const aeron::concurrent::AtomicBuffer& buf,
                       aeron::index_t offset,
                       aeron::index_t length,
                       const aeron::Header& /*hdr*/) {
        if (found)
            return;  // already satisfied — drain remaining fragments

        // Decode the SBE message header to verify template ID.
        if (static_cast<std::size_t>(length) < MessageHeader::encodedLength() + BacktestAck::sbeBlockLength())
            return;

        char* raw = reinterpret_cast<char*>(const_cast<uint8_t*>(buf.buffer())) + offset;
        MessageHeader hdr(raw, static_cast<uint64_t>(length));
        if (hdr.templateId() != BacktestAck::sbeTemplateId())
            return;

        BacktestAck ack;
        ack.wrapForDecode(raw,
                          MessageHeader::encodedLength(),
                          hdr.blockLength(),
                          hdr.version(),
                          static_cast<uint64_t>(length));

        if (ack.tickSeqNum() == expected_seq)
            found = true;
    };

    while (!found && std::chrono::steady_clock::now() < deadline) {
        int fragments = sub_->poll(handler, 10);
        if (fragments == 0)
            std::this_thread::sleep_for(10us);
    }

    return found;
}

}  // namespace bpt::backtester::messaging
