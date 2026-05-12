#pragma once

/// \file
/// \brief Sorted (price → qty) ladder backed by std::vector.
///
/// Replaces std::map<double, double, Compare> for per-instrument book
/// state in the venue decoders. Same logical API (insert/update by
/// price, erase on qty==0, sorted iteration) but the storage is a
/// contiguous vector instead of red-black-tree nodes scattered across
/// the heap.
///
/// Why this is faster on the typical book-feed workload:
///   - Top-of-book modifications (the common case — best bid/ask qty
///     changes) hit index 0 of a contiguous array; std::map walks the
///     tree to leftmost.
///   - No per-level heap allocation. std::map allocates one node per
///     price level on insert; this vector reuses storage across calls.
///   - Iteration is a linear scan over contiguous memory; the prefetcher
///     loves it. std::map's tree walk is cache-cold.
///
/// What's worse than std::map: insertion / deletion of NEW levels into
/// the middle of the ladder is O(n) memmove instead of O(log n) tree
/// rotations. At n=400 (OKX `books` full ladder) the shift is ~6 KB
/// memmove (~640 ns), which is still competitive with the cache-cold
/// tree walks std::map would do, and the tradeoff is favourable on the
/// modify-existing-level workload that dominates real feeds.

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace bpt::md_gateway::md {

/// \brief Sorted ladder of (price, qty) pairs ordered by `Compare` on price.
///
/// Use `std::greater<>` to keep best bid first; default `std::less<>`
/// keeps best ask first. NOT thread-safe — one instance per writer
/// thread (matches the existing single-writer book-state contract).
template <class Compare = std::less<double>>
class SortedLadder {
public:
    using Level = std::pair<double, double>;  ///< (price, qty)

    /// Pre-allocate backing storage. Optional but pays off if you know
    /// the typical depth (e.g. 400 for OKX `books`) — avoids reallocs
    /// during the first warmup snapshot.
    void reserve(std::size_t n) { data_.reserve(n); }

    [[nodiscard]] bool empty() const noexcept { return data_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
    void clear() noexcept { data_.clear(); }

    /// \brief Apply a single (price, qty) update with delta semantics.
    ///
    /// qty == 0 removes the level (no-op if not present); qty > 0
    /// inserts a new level or updates the qty of an existing one.
    void apply(double price, double qty) {
        auto it = std::lower_bound(data_.begin(), data_.end(), price, [](const Level& lvl, double px) {
            return Compare{}(lvl.first, px);
        });
        const bool found = (it != data_.end() && it->first == price);
        if (qty == 0.0) {
            if (found)
                data_.erase(it);
        } else if (found) {
            it->second = qty;
        } else {
            data_.insert(it, Level{price, qty});
        }
    }

    auto begin() noexcept { return data_.begin(); }
    auto end() noexcept { return data_.end(); }
    auto begin() const noexcept { return data_.begin(); }
    auto end() const noexcept { return data_.end(); }
    auto cbegin() const noexcept { return data_.cbegin(); }
    auto cend() const noexcept { return data_.cend(); }

    /// Direct top-of-book accessor. Undefined if empty(); callers check first.
    [[nodiscard]] const Level& front() const noexcept { return data_.front(); }

private:
    std::vector<Level> data_;
};

}  // namespace bpt::md_gateway::md
