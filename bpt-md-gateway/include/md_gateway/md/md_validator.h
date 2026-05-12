#pragma once

/// \file
/// \brief Per-instrument sanity checks on normalised MD before SBE encode.
///
/// Sits between adapter decoders and the publisher. Catches the obvious
/// failure modes — zero / negative prices, crossed books, out-of-range
/// jumps from the last validated mid — that signal either a venue
/// schema drift or a bug in the adapter's parsing layer. Drops are
/// silent at this level; ValidationDropBreaker (one layer up) latches
/// when too many drops accumulate.

#include "md_gateway/md/md_types.h"

#include <cstdint>
#include <unordered_map>

namespace bpt::md_gateway::md {

/// \brief Outcome of one validation call.
enum class ValidationResult {
    OK,   ///< message passes all checks; safe to publish
    DROP  ///< at least one check failed; caller MUST NOT publish
};

/// \brief Validates normalised MD structs before SBE encoding.
///
/// Checks performed:
///   - **BBO**: price > 0, qty > 0, ask > bid (no crossed book),
///     mid deviation from last validated mid <= threshold.
///   - **Trade**: price > 0, qty > 0,
///     deviation from last validated BBO mid <= threshold.
///   - **OrderBook**: non-empty sides, best ask > best bid,
///     bids descending, asks ascending, best-mid deviation from last
///     validated mid <= threshold.
///
/// Per-instrument last-mid is updated only when a BBO passes validation.
/// OrderBook validation reads it but does not write to it (the adapter
/// always follows an OrderBook publish with a derived BBO).
///
/// Not thread-safe — one instance per adapter (publisher) thread.
class MdValidator {
public:
    /// \brief Construct.
    /// \param max_price_deviation_pct percentage threshold for the
    ///        per-instrument mid-deviation guard. E.g. 10.0 rejects any
    ///        tick whose mid moves more than 10% from the last validated
    ///        value. Set to 0 to disable the check.
    explicit MdValidator(double max_price_deviation_pct = 10.0);

    [[nodiscard]] ValidationResult validate(const MdBbo& bbo);
    [[nodiscard]] ValidationResult validate(const MdTrade& trade);
    [[nodiscard]] ValidationResult validate(const MdOrderBook& book);

    /// \brief Forget all per-instrument state — call on adapter reconnect.
    void reset();

private:
    double max_deviation_ratio_;
    std::unordered_map<uint64_t, double> last_mid_;      ///< instrument_id → last validated mid
    std::unordered_map<uint64_t, uint32_t> drop_count_;  ///< per-instrument log-spam throttle
};

}  // namespace bpt::md_gateway::md
