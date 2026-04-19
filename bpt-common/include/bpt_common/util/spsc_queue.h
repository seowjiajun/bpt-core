#pragma once

// yggdrasil/spsc_queue.h — Lock-free single-producer / single-consumer ring buffer.
//
// Dependencies: none (C++ stdlib only).
//
// Usage:
//   bpt::common::util::SpscQueue<512, 16384> q;
//   // producer thread:
//   q.try_push(recv_ns, payload);
//   // consumer thread:
//   q.try_pop([](uint64_t recv_ns, std::string_view data) { ... });

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace bpt::common::util {

// Lock-free single-producer / single-consumer ring buffer for raw byte frames.
//
// Typical use: decouple a WebSocket IO thread (producer) from a parser/publisher
// thread (consumer).  The IO thread stamps recv_ns and enqueues the raw payload;
// the consumer drains, parses, and offers to Aeron — never blocking the IO thread
// on Aeron back-pressure.
//
// CAPACITY must be a power of 2.
// MAX_PAYLOAD_BYTES is the per-slot limit — oversized frames are dropped.
//
// Memory: CAPACITY * (16 + MAX_PAYLOAD_BYTES) bytes, plus two cache-line-aligned
// atomic counters.  E.g. 512 slots × 16 KiB ≈ 8 MiB per queue.
template <size_t CAPACITY, size_t MAX_PAYLOAD_BYTES>
class SpscQueue {
    static_assert((CAPACITY & (CAPACITY - 1)) == 0, "CAPACITY must be a power of 2");
    static constexpr size_t MASK = CAPACITY - 1;

    struct Slot {
        uint64_t recv_ns{0};
        size_t len{0};
        char data[MAX_PAYLOAD_BYTES];
    };

public:
    SpscQueue() = default;

    // Called by the producer thread only.
    // Returns false if the queue is full or payload exceeds MAX_PAYLOAD_BYTES.
    [[nodiscard]] bool try_push(uint64_t recv_ns, std::string_view payload) noexcept {
        if (payload.size() > MAX_PAYLOAD_BYTES)
            return false;
        const size_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) >= CAPACITY)
            return false;
        Slot& s = slots_[h & MASK];
        s.recv_ns = recv_ns;
        s.len = payload.size();
        std::memcpy(s.data, payload.data(), payload.size());
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    // Called by the consumer thread only.
    // Invokes fn(recv_ns, string_view) while the slot is still owned, then
    // releases it.  Returns false if the queue is empty.
    template <typename Fn>
    bool try_pop(Fn&& fn) noexcept {
        const size_t t = tail_.load(std::memory_order_relaxed);
        if (head_.load(std::memory_order_acquire) == t)
            return false;
        const Slot& s = slots_[t & MASK];
        fn(s.recv_ns, std::string_view(s.data, s.len));
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    static constexpr size_t capacity() noexcept { return CAPACITY; }
    static constexpr size_t max_payload_bytes() noexcept { return MAX_PAYLOAD_BYTES; }

private:
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    Slot slots_[CAPACITY];
};

}  // namespace bpt::common::util
