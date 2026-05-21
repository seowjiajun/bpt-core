#pragma once

#include <messages/MdOrderBook.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <utility>
#include <vector>

namespace bpt::features {

// Per-instrument L2 book state built by folding MdOrderBook frames into
// a maintained ladder. OKX `books` and `books5` feeds both publish
// incremental updates — a frame carries only changed levels, with qty=0
// meaning "remove this level". The ladder therefore has to be stateful:
// applying a single MdOrderBook frame gives you only the just-changed
// levels, not the full book.
//
// Bids are stored descending (best first), asks ascending. Price/qty
// types match the SBE schema (both `double` in MdOrderBook — the encoder
// writes raw doubles, see md_encoder.cpp).
//
// Not thread-safe — one instance per instrument, meant to be fed from
// the MdClient poll thread.
class OrderBookState {
public:
    struct Level {
        double price;
        double qty;
    };

    // Fold a new MdOrderBook frame into the ladder.
    //
    // When is_snapshot is false (default — delta semantics): qty>0 sets
    // the level, qty==0 removes it. Levels NOT mentioned in the frame
    // remain in the ladder untouched. This matches OKX `books` (depth>5)
    // and similar delta-only channels.
    //
    // When is_snapshot is true: the ladder is cleared before folding.
    // Use this for channels that publish full-book snapshots every frame
    // (OKX `books5`, `bbo-tbt`). Without this flag, stale levels that
    // drop out of the top-N window stay in the ladder forever because
    // snapshots never emit qty=0 removals for them.
    //
    // Updates last_seq_num_ and last_update_ns_ regardless.
    void apply(const bpt::messages::MdOrderBook& book, bool is_snapshot = false);

    // Low-level overload used by the SBE path above and by unit tests.
    void apply(const std::vector<Level>& bid_levels,
               const std::vector<Level>& ask_levels,
               uint64_t seq_num,
               uint64_t timestamp_ns,
               bool is_snapshot = false);

    // Reset the ladder (e.g. on detected sequence gap or adapter
    // reconnect). Callers should force a re-subscription to get a fresh
    // snapshot before resuming.
    void reset();

    // Top-of-book accessors. Undefined if ready() is false — callers must
    // check first.
    double best_bid() const;
    double best_ask() const;
    double best_bid_qty() const;
    double best_ask_qty() const;
    double mid() const;

    // Exact-price lookups. Returns 0.0 if the price is not in the ladder.
    double size_at_bid(double price) const;
    double size_at_ask(double price) const;

    // Cumulative volume ahead of a given price in the FIFO priority sense:
    //  - bid_vol_above(p) sums qty at all bid prices STRICTLY greater than p
    //  - ask_vol_below(p) sums qty at all ask prices STRICTLY less than p
    // Used by the queue tracker to estimate fill probability for a resting
    // order at price p.
    double bid_vol_above(double price) const;
    double ask_vol_below(double price) const;

    // Top N levels, ordered best → worst. If N exceeds the ladder depth,
    // the shorter actual depth is returned.
    //
    // The buffer-fill overloads (preferred on the hot path) `.clear()` `out`
    // and refill — once the caller has called `.reserve(N)` at warmup,
    // these allocate zero on the per-tick path. The value-return versions
    // are kept for tests + callers where allocation cost is irrelevant.
    void top_bids(size_t n, std::vector<Level>& out) const;
    void top_asks(size_t n, std::vector<Level>& out) const;
    std::vector<Level> top_bids(size_t n) const;
    std::vector<Level> top_asks(size_t n) const;

    size_t n_bid_levels() const { return bids_.size(); }
    size_t n_ask_levels() const { return asks_.size(); }
    uint64_t last_seq_num() const { return last_seq_num_; }
    uint64_t last_update_ns() const { return last_update_ns_; }

    // True once both sides have at least one level — i.e. a mid price is
    // defined. Strategy logic should gate on this before reading any
    // accessor beyond the size_at_* / vol_* helpers (which return 0 on
    // missing levels and so are always safe).
    bool ready() const { return !bids_.empty() && !asks_.empty(); }

private:
    // Bids descending: std::greater<> so .begin() is the best bid.
    std::map<double, double, std::greater<double>> bids_;
    // Asks ascending: default less<> so .begin() is the best ask.
    std::map<double, double> asks_;
    uint64_t last_seq_num_{0};
    uint64_t last_update_ns_{0};
};

}  // namespace bpt::features
