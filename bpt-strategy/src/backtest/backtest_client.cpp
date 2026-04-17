#include "strategy/backtest/backtest_client.h"

#include <messages/BacktestAck.h>
#include <messages/BacktestControl.h>
#include <messages/MessageHeader.h>

#include <chrono>
#include <stdexcept>
#include <thread>
#include <x86intrin.h>
#include <yggdrasil/logging.h>

using namespace bpt::messages;

namespace bpt::strategy::backtest {

BacktestClient::BacktestClient(std::shared_ptr<aeron::Aeron> aeron,
                               const std::string& channel,
                               int32_t control_stream_id,
                               int32_t ack_stream_id,
                               int pub_timeout_ms,
                               int pub_poll_interval_ms) {
    long ctrl_id = aeron->addSubscription(channel, control_stream_id);
    long ack_id = aeron->addPublication(channel, ack_stream_id);

    const int max_retries = pub_timeout_ms / std::max(pub_poll_interval_ms, 1);
    const auto poll_interval = std::chrono::milliseconds(pub_poll_interval_ms);

    int retries = 0;
    while (!(ctrl_sub_ = aeron->findSubscription(ctrl_id))) {
        if (++retries > max_retries)
            throw std::runtime_error("[BacktestClient] timed out waiting for BacktestControl subscription");
        std::this_thread::sleep_for(poll_interval);
    }
    retries = 0;
    while (!(ack_pub_ = aeron->findPublication(ack_id))) {
        if (++retries > max_retries)
            throw std::runtime_error("[BacktestClient] timed out waiting for BacktestAck publication");
        std::this_thread::sleep_for(poll_interval);
    }

    ygg::log::info("[BacktestClient] connected: ctrl_sub={} ack_pub={}", control_stream_id, ack_stream_id);
}

void BacktestClient::send_ack(uint64_t tick_seq, uint64_t simulation_ts) {
    BacktestAck msg;
    msg.wrapAndApplyHeader(ack_buf_, 0, kAckBufSize).tickSeqNum(tick_seq).simulationTs(simulation_ts);

    aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(ack_buf_), static_cast<aeron::util::index_t>(kAckBufSize));
    const auto len = static_cast<aeron::util::index_t>(kAckBufSize);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    long result;
    while ((result = ack_pub_->offer(ab, 0, len)) < 0) {
        if (result == aeron::PUBLICATION_CLOSED)
            throw std::runtime_error("[BacktestClient] ack publication closed");
        if (std::chrono::steady_clock::now() > deadline)
            throw std::runtime_error("[BacktestClient] ack offer timed out (back-pressured for >1s)");
        _mm_pause();
    }
}

int BacktestClient::poll() {
    return ctrl_sub_->poll(
        [this](aeron::AtomicBuffer& buffer,
               aeron::util::index_t offset,
               aeron::util::index_t length,
               aeron::Header& /*hdr*/) {
            if (static_cast<std::size_t>(length) < MessageHeader::encodedLength())
                return;

            char* data = reinterpret_cast<char*>(buffer.buffer()) + offset;
            MessageHeader hdr(data, static_cast<std::size_t>(length));

            if (hdr.templateId() != BacktestControl::sbeTemplateId())
                return;

            BacktestControl msg;
            msg.wrapForDecode(data,
                              MessageHeader::encodedLength(),
                              hdr.blockLength(),
                              hdr.version(),
                              static_cast<std::size_t>(length));

            if (on_control)
                on_control(msg.command(), msg.tickSeqNum(), msg.simulationTs());
        },
        10);
}

}  // namespace bpt::strategy::backtest
