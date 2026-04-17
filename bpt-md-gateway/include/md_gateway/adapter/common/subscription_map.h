#pragma once

#include <cstdint>
#include <functional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bpt::md_gateway::adapter {

// Transparent hash/equal for std::unordered_map<std::string, ...> heterogeneous lookup.
// Allows find(std::string_view) without constructing a std::string on the hot path.
struct StringViewHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view sv) const noexcept { return std::hash<std::string_view>{}(sv); }
    std::size_t operator()(const std::string& s) const noexcept { return std::hash<std::string_view>{}(s); }
};

// Thread-safe registry of active instrument subscriptions.
//
// subscribe() / unsubscribe() may be called from any thread.
// find_id() and find_depth() are on the hot receive path and take only a shared
// (reader) lock — they do not block concurrent readers.
// take_pending() is called by each adapter's read loop to drain runtime
// subscription requests that arrived after the last connect.
class SubscriptionMap {
public:
    struct Entry {
        std::string symbol;
        uint8_t depth{0};
    };

    // Register a new subscription (or update depth for an existing one).
    // Appends to the pending queue so the read loop can send a subscribe frame.
    void subscribe(uint64_t instrument_id, std::string symbol, uint8_t depth = 0);

    // Remove a subscription. Returns the exchange symbol that was removed,
    // or an empty string if instrument_id was not found.
    std::string unsubscribe(uint64_t instrument_id);

    // Re-queue an existing instrument for resubscription (e.g. after an order
    // book gap). Only adds to pending if the instrument is currently subscribed.
    void requeue(const std::string& symbol);

    // Look up the canonical instrument ID by exchange symbol.
    // Accepts string_view — no string construction on the hot path.
    // Returns 0 if not found. Hot path — shared lock only.
    [[nodiscard]] uint64_t find_id(std::string_view symbol) const;

    // Look up the subscribed depth for an instrument by ID.
    // Returns 0 if not found. Hot path — shared lock only.
    [[nodiscard]] uint8_t find_depth(uint64_t instrument_id) const;

    struct FindResult {
        uint64_t instrument_id{0};
        uint8_t depth{0};
    };

    // Combined lookup: returns instrument_id + depth in a single lock acquisition.
    // Use this on the hot path instead of calling find_id() + find_depth() separately.
    // Returns {0, 0} if not found.
    [[nodiscard]] FindResult find(std::string_view symbol) const;

    // Snapshot all current subscriptions. Called on (re)connect to send
    // the full set of initial subscribe frames.
    [[nodiscard]] std::vector<std::pair<uint64_t, Entry>> snapshot() const;

    // Atomically drain and return subscriptions added since the last connect.
    std::vector<Entry> take_pending();

private:
    mutable std::shared_mutex mu_;
    std::unordered_map<uint64_t, Entry> by_id_;
    std::unordered_map<std::string, uint64_t, StringViewHash, std::equal_to<>> by_symbol_;
    std::vector<Entry> pending_;
};

}  // namespace bpt::md_gateway::adapter
