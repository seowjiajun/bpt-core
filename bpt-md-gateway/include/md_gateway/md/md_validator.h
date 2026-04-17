#pragma once

#include "md_gateway/md/md_types.h"

#include <cstdint>
#include <unordered_map>

namespace bpt::md_gateway::md {

enum class ValidationResult { OK, DROP };

// Validates normalised market-data structs before SBE encoding.
//
// Checks performed:
//   BBO:       price > 0, qty > 0, ask > bid (no crossed book),
//              price deviation from last known mid <= threshold.
//   Trade:     price > 0, qty > 0,
//              price deviation from last known BBO mid <= threshold.
//   OrderBook: non-empty sides, best ask > best bid,
//              bids descending, asks ascending,
//              best-mid deviation from last known mid <= threshold.
//
// Per-instrument last-mid price is updated only when a BBO passes validation.
// OrderBook validation checks against it but does not update it (the adapter
// always follows an OrderBook publish with a BBO derived from the top of book).
//
// Not thread-safe — one instance per adapter (publisher) thread.
class MdValidator {
public:
    // max_price_deviation_pct: percentage threshold for the per-instrument
    // price-deviation guard.  E.g. 10.0 rejects any tick that moves the mid
    // price more than 10% from the last known value.
    explicit MdValidator(double max_price_deviation_pct = 10.0);

    [[nodiscard]] ValidationResult validate(const MdBbo& bbo);
    [[nodiscard]] ValidationResult validate(const MdTrade& trade);
    [[nodiscard]] ValidationResult validate(const MdOrderBook& book);

    // Reset per-instrument state (call on reconnect).
    void reset();

private:
    double max_deviation_ratio_;
    std::unordered_map<uint64_t, double> last_mid_;      // instrument_id → last validated mid price
    std::unordered_map<uint64_t, uint32_t> drop_count_;  // throttle repeated log spam per instrument
};

}  // namespace bpt::md_gateway::md
