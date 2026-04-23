#pragma once

#include "md_gateway/md/md_encoder.h"
#include "md_gateway/messaging/i_md_publisher.h"

#include <Aeron.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <bpt_common/aeron/publisher.h>

namespace bpt::md_gateway::messaging {

// Publishes normalised market-data structs on the MdGateway→Strategy data stream
// (stream 2002).
//
// Encoding is delegated to MdEncoder (struct → SBE bytes into a stack buffer).
// Only the Aeron offer() call lives here, keeping transport fully decoupled
// from the SBE schema.
//
// Stack buffers are intentionally NOT zero-initialised: MdEncoder writes every
// byte it uses and offer() reads only `len` bytes, so zeroing is wasted work.
// A pre-allocated member buffer would be a data race — multiple adapter
// publisher threads share this instance.
//
// Thread-safe: multiple adapter threads may call publish() concurrently.
// aeron::Publication::offer() uses an internal CAS; seq_ is incremented via
// fetch_add (relaxed — each message carries its own sequence number).
class MdPublisher : public IMdPublisher {
public:
    MdPublisher(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int stream_id);

    void publish(const md::MdBbo& bbo) override;
    void publish(const md::MdTrade& trade) override;
    void publish(const md::MdOrderBook& book) override;

    [[nodiscard]] uint64_t current_seq() const { return seq_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t drop_count() const { return drops_.load(std::memory_order_relaxed); }

private:
    void offer(const char* buf, std::size_t len, uint64_t instrument_id, const char* label);

    bpt::common::aeron::Publisher publisher_;
    std::atomic<uint64_t> seq_{0};
    std::atomic<uint64_t> drops_{0};
};

}  // namespace bpt::md_gateway::messaging
