// pybind11 bindings for bpt-features. Lets research notebooks compute
// features by calling into the SAME C++ implementation that production
// strategies use — the production-research drift fix from the feature-
// library design discussion.
//
// One binding per feature class. Each class gets its raw C++ API
// exposed; thin function-style Python wrappers (microprice(df) →
// Series) live in bpt_features/*.py and call into these classes.
//
// Naming: the compiled module is `_core` (leading underscore signals
// "internal C extension, don't import directly"); the public Python
// package `bpt_features` re-exports the bits people should use.

#include "bpt_common/book/order_book_state.h"
#include "features/ewma.h"
#include "features/fair_value.h"
#include "features/fill_prob.h"
#include "features/hurst.h"
#include "features/ofi.h"
#include "features/queue.h"
#include "features/realized_vol.h"
#include "features/regime_classifier.h"
#include "features/regime_detector.h"
#include "features/vol_gate.h"

#include <messages/OrderSide.h>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>  // for std::vector ↔ Python list conversion

namespace py = pybind11;
using bpt::common::book::OrderBookState;
using bpt::features::compute_hurst;
using bpt::features::compute_hurst_multi_window;
using bpt::features::EwmaDrift;
using bpt::features::EwmaVariance;
using bpt::features::FairValueEstimator;
using bpt::features::KappaEstimator;
using bpt::features::OFICalculator;
using bpt::features::QueueTracker;
using bpt::features::RealizedVolEstimator;
using bpt::features::RegimeClassifier;
using bpt::features::RegimeDetector;
using bpt::features::TimeWeightedEwma;
using bpt::features::VolatilityGate;
using bpt::messages::OrderSide;

PYBIND11_MODULE(_core, m) {
    m.doc() = "bpt-features C++ extension — compiled from bpt-features/python/bindings.cpp";

    // ─────────────────────────── OFICalculator ───────────────────────────
    // Cont-Kukanov-Stoikov rolling Order-Flow Imbalance. See
    // features/ofi.h for the math + per-level weighting rules.
    py::class_<OFICalculator> ofi(m, "OFICalculator", "Rolling multi-level Order-Flow Imbalance estimator");

    py::class_<OFICalculator::Config>(ofi, "Config")
        .def(py::init<>())
        .def_readwrite("max_levels", &OFICalculator::Config::max_levels)
        .def_readwrite("window_ns", &OFICalculator::Config::window_ns);

    ofi.def(py::init<OFICalculator::Config>(), py::arg("cfg"))
        // update() is the hot path. The (price, qty) Level pairs are
        // std::pair<double, double>; pybind11/stl.h auto-converts a
        // Python list-of-tuples into the C++ vector representation.
        .def("update",
             &OFICalculator::update,
             py::arg("bids"),
             py::arg("asks"),
             py::arg("timestamp_ns"),
             "Feed a book snapshot, return the current rolling OFI value")
        .def("is_warm", &OFICalculator::is_warm, "True once at least two updates have produced a value")
        .def("value", &OFICalculator::value, "Last value returned by update(); 0 if never called")
        .def("avg_depth", &OFICalculator::avg_depth, "Rolling average top-K depth (bid+ask) over the window")
        .def("reset", &OFICalculator::reset, "Reset all internal state — use on reconnect");

    // ────────────────────────── FairValueEstimator ───────────────────────
    // Reference-price estimator family. Mode picks the algorithm:
    // mid, micro (Stoikov), micro-size-capped, L2-weighted, EWMA-micro.
    // Python wrappers in fair_value.py expose one function per mode
    // so notebooks don't have to manage the Mode enum directly.
    //
    // NOTE: the OrderBookState overload of estimate() is omitted — that
    // path requires constructing OBS from Python which is bigger scope.
    // Only the top-of-book overload is exposed here, which means
    // kL2WeightedMicro silently degrades to kMicro in Python (same
    // behaviour as the C++ TOB-only path; documented in fair_value.h).
    py::class_<FairValueEstimator> fve(m,
                                       "FairValueEstimator",
                                       "Reference-price estimator (mid / micro / EWMA variants)");

    py::enum_<FairValueEstimator::Mode>(fve, "Mode")
        .value("Mid", FairValueEstimator::Mode::kMid)
        .value("Micro", FairValueEstimator::Mode::kMicro)
        .value("MicroSizeCapped", FairValueEstimator::Mode::kMicroSizeCapped)
        .value("L2WeightedMicro", FairValueEstimator::Mode::kL2WeightedMicro)
        .value("EwmaMicro", FairValueEstimator::Mode::kEwmaMicro);

    py::class_<FairValueEstimator::Config>(fve, "Config")
        .def(py::init<>())
        .def_readwrite("mode", &FairValueEstimator::Config::mode)
        .def_readwrite("size_cap_qty", &FairValueEstimator::Config::size_cap_qty)
        .def_readwrite("ladder_depth", &FairValueEstimator::Config::ladder_depth)
        .def_readwrite("ladder_decay", &FairValueEstimator::Config::ladder_decay)
        .def_readwrite("ewma_alpha", &FairValueEstimator::Config::ewma_alpha);

    fve.def(py::init<>())
        .def(py::init<FairValueEstimator::Config>(), py::arg("cfg"))
        // TOB-only overload — selected via the overload that takes 4 doubles.
        // pybind11 picks this by argument count + types.
        .def("estimate",
             py::overload_cast<double, double, double, double>(&FairValueEstimator::estimate),
             py::arg("bid_px"),
             py::arg("ask_px"),
             py::arg("bid_qty"),
             py::arg("ask_qty"),
             "Compute fair value from top-of-book. Returns NaN on degenerate quote.")
        .def("last_estimate",
             &FairValueEstimator::last_estimate,
             "Last computed value; NaN if estimate() never called or book was unready")
        .def("mode", &FairValueEstimator::mode, "Current mode");

    // ───────────────────────── RealizedVolEstimator ──────────────────────
    // Rolling-window annualised realised vol from mid-price returns.
    // Sampled at fixed intervals (sample_interval_ns) so the rate is
    // deterministic regardless of tick density.
    py::class_<RealizedVolEstimator>(m, "RealizedVolEstimator", "Rolling annualised realised vol estimator")
        .def(py::init<size_t, uint64_t>(),
             py::arg("window_size"),
             py::arg("sample_interval_ns"),
             "window_size: number of samples in the rolling window. "
             "sample_interval_ns: minimum spacing between samples (ticks closer than "
             "this are ignored).")
        .def("update",
             &RealizedVolEstimator::update,
             py::arg("mid_price"),
             py::arg("timestamp_ns"),
             "Feed a mid-price tick. Returns True if this tick was accepted as a sample "
             "(i.e. enough time has elapsed since the last accepted sample).")
        .def("realized_vol",
             &RealizedVolEstimator::realized_vol,
             "Current realised vol (annualised, in price units). 0 until ready().")
        .def("count", &RealizedVolEstimator::count, "Number of samples currently in the rolling window.")
        .def("ready", &RealizedVolEstimator::ready, "True once min_samples samples have been collected.")
        .def("reset", &RealizedVolEstimator::reset, "Reset all internal state.");

    // ─────────────────────────── VolatilityGate ──────────────────────────
    // Suppression gate that trips on a short-horizon mid-price move
    // exceeding `max_bps_per_window`. While tripped, is_halted() returns
    // true and the strategy should suppress quoting.
    py::class_<VolatilityGate> volgate(m, "VolatilityGate", "Vol-spike suppression gate (trips on rapid mid moves)");

    py::class_<VolatilityGate::Config>(volgate, "Config")
        .def(py::init<>())
        .def_readwrite("max_bps_per_window", &VolatilityGate::Config::max_bps_per_window);

    volgate.def(py::init<VolatilityGate::Config>(), py::arg("cfg"))
        .def("update_and_check",
             &VolatilityGate::update_and_check,
             py::arg("mid"),
             py::arg("now_ns"),
             "Feed a mid-price tick. Returns True if the gate is currently HALTED.")
        .def("is_halted",
             &VolatilityGate::is_halted,
             py::arg("now_ns"),
             "True if the gate is currently in its post-trip halt window.")
        .def("last_trip_bps",
             &VolatilityGate::last_trip_bps,
             "Bps move that triggered the most recent trip (0 if never tripped).")
        .def("enabled", &VolatilityGate::enabled, "True if max_bps_per_window > 0 (gate is configured to fire).")
        .def("max_bps_per_window", &VolatilityGate::max_bps_per_window)
        .def("set_max_bps_per_window", &VolatilityGate::set_max_bps_per_window, py::arg("max_bps"));

    // ─────────────────────────── OrderSide enum ──────────────────────────
    // Wire-format enum from messages/OrderSide.h. Exposed here so
    // QueueTracker callers can pass `bf.OrderSide.Buy` / `.Sell` instead
    // of raw ints. NULL_VALUE is the SBE sentinel; not typically used
    // from Python but exposed for completeness.
    py::enum_<OrderSide::Value>(m, "OrderSide")
        .value("Buy", OrderSide::BUY)
        .value("Sell", OrderSide::SELL)
        .value("Null", OrderSide::NULL_VALUE);

    // ────────────────────────── OrderBookState ───────────────────────────
    // Per-instrument L2 book state. Lives in bpt-common as a shared
    // domain type (md-gateway, tape, backtester all plausibly consume).
    // Bound here because the features that use it (FairValueEstimator's
    // L2 mode, QueueTracker) are the typical Python consumers; keeping
    // all the Python bindings in one .so avoids forcing bpt-common to
    // grow its own pybind11 target.
    //
    // The MdOrderBook overload of apply() is NOT bound — that path is
    // for the live md-gateway adapter feeding SBE-decoded frames; from
    // Python you construct ladders directly via the vector overload.
    py::class_<OrderBookState> obs(m, "OrderBookState", "Stateful L2 order book ladder");

    py::class_<OrderBookState::Level>(obs, "Level")
        .def(py::init<>())
        .def_readwrite("price", &OrderBookState::Level::price)
        .def_readwrite("qty", &OrderBookState::Level::qty);

    obs.def(py::init<>())
        // The vector overload — research-friendly. Pass lists of Levels;
        // pybind11/stl.h converts Python list-of-Level to C++ vector.
        // is_snapshot=true wipes the ladder before applying (use for
        // full-book snapshots like OKX books5); false (default) is
        // delta semantics (qty=0 removes a level).
        .def("apply",
             py::overload_cast<const std::vector<OrderBookState::Level>&,
                               const std::vector<OrderBookState::Level>&,
                               uint64_t,
                               uint64_t,
                               bool>(&OrderBookState::apply),
             py::arg("bid_levels"),
             py::arg("ask_levels"),
             py::arg("seq_num"),
             py::arg("timestamp_ns"),
             py::arg("is_snapshot") = false,
             "Fold a book frame into the ladder.")
        .def("reset", &OrderBookState::reset, "Clear all state.")
        .def("ready", &OrderBookState::ready, "True once both sides have at least one level.")
        // Top-of-book accessors — undefined if !ready().
        .def("best_bid", &OrderBookState::best_bid)
        .def("best_ask", &OrderBookState::best_ask)
        .def("best_bid_qty", &OrderBookState::best_bid_qty)
        .def("best_ask_qty", &OrderBookState::best_ask_qty)
        .def("mid", &OrderBookState::mid)
        // Exact-price + cumulative lookups (always safe).
        .def("size_at_bid", &OrderBookState::size_at_bid, py::arg("price"))
        .def("size_at_ask", &OrderBookState::size_at_ask, py::arg("price"))
        .def("bid_vol_above",
             &OrderBookState::bid_vol_above,
             py::arg("price"),
             "Sum of qty at all bid prices STRICTLY greater than `price`.")
        .def("ask_vol_below",
             &OrderBookState::ask_vol_below,
             py::arg("price"),
             "Sum of qty at all ask prices STRICTLY less than `price`.")
        // Value-return top-N accessors. (The buffer-fill overloads are
        // hot-path optimisations not relevant to Python.)
        .def("top_bids",
             py::overload_cast<size_t>(&OrderBookState::top_bids, py::const_),
             py::arg("n"),
             "Top n bid levels, best first.")
        .def("top_asks",
             py::overload_cast<size_t>(&OrderBookState::top_asks, py::const_),
             py::arg("n"),
             "Top n ask levels, best first.")
        .def("n_bid_levels", &OrderBookState::n_bid_levels)
        .def("n_ask_levels", &OrderBookState::n_ask_levels)
        .def("last_seq_num", &OrderBookState::last_seq_num)
        .def("last_update_ns", &OrderBookState::last_update_ns);

    // ─────────────────────────── QueueTracker ────────────────────────────
    // Per-resting-order queue-position tracker. Lifecycle is event-driven:
    //   track(...)       — register a new resting order
    //   on_trade(...)    — public trade arrived, decrement queue_ahead
    //   on_fill(...)     — our order filled (partial or full)
    //   on_cancel(...)   — our order cancelled
    //   fill_probability(...) — read the current signal
    //
    // No per-row function wrapper today — the call pattern is too
    // event-shaped for a simple DataFrame mapping. Once we have a canon
    // reader that yields (book_state, my_fills, trades) streams together,
    // a `bf.queue_position_history(...)` wrapper becomes natural.
    py::class_<QueueTracker> qt(m, "QueueTracker", "Estimates queue position for resting orders");

    py::class_<QueueTracker::Entry>(qt, "Entry")
        .def_readonly("side", &QueueTracker::Entry::side)
        .def_readonly("price", &QueueTracker::Entry::price)
        .def_readonly("our_qty", &QueueTracker::Entry::our_qty)
        .def_readonly("queue_ahead", &QueueTracker::Entry::queue_ahead)
        .def_readonly("placed_ns", &QueueTracker::Entry::placed_ns);

    qt.def(py::init<>())
        .def("track",
             &QueueTracker::track,
             py::arg("order_id"),
             py::arg("side"),
             py::arg("price"),
             py::arg("our_qty"),
             py::arg("ts_ns"),
             py::arg("book"),
             "Register a newly acked resting order; snapshots queue_ahead from the current book.")
        .def("on_fill", &QueueTracker::on_fill, py::arg("order_id"), py::arg("filled_qty"))
        .def("on_cancel", &QueueTracker::on_cancel, py::arg("order_id"))
        .def("on_trade",
             &QueueTracker::on_trade,
             py::arg("aggressor"),
             py::arg("trade_price"),
             py::arg("trade_qty"),
             py::arg("ts_ns"),
             "Public-market trade printed; decrements queue_ahead for matching passive entries.")
        // lookup() returns a pointer that may be nullptr; pybind11 needs
        // an explicit reference-policy + nullable-check. Use a lambda
        // that returns Optional[Entry] instead.
        .def(
            "lookup",
            [](const QueueTracker& self, uint64_t order_id) -> py::object {
                const auto* e = self.lookup(order_id);
                if (e == nullptr)
                    return py::none();
                return py::cast(*e);  // copy the Entry into Python
            },
            py::arg("order_id"))
        .def("fill_probability",
             &QueueTracker::fill_probability,
             py::arg("order_id"),
             "p = our_qty / (our_qty + queue_ahead). 0 if order_id unknown.")
        .def("size", &QueueTracker::size, "Number of tracked entries.");

    // ─── EWMA estimators (extracted from AS) ──────────────────────────────
    //
    // Same C++ AS uses on the tick path. Streaming API: feed (mid, ts_ns)
    // each tick, query value() and count() for warmup. KappaEstimator
    // takes just a trade timestamp.

    py::class_<TimeWeightedEwma>(m, "TimeWeightedEwma", "Time-weighted EWMA primitive. λ = exp(-dt_s / halflife_s).")
        .def(py::init<double>(), py::arg("halflife_s"))
        .def("update", &TimeWeightedEwma::update, py::arg("obs"), py::arg("dt_s"))
        .def("reset", &TimeWeightedEwma::reset)
        .def("value", &TimeWeightedEwma::value)
        .def("count", &TimeWeightedEwma::count)
        .def("halflife_s", &TimeWeightedEwma::halflife_s);

    py::class_<EwmaVariance>(m,
                             "EwmaVariance",
                             "EWMA of squared time-normalised log returns — per-second variance σ². "
                             "Observation each tick: (log(mid/last_mid)/sqrt(dt_s))².")
        .def(py::init<double>(), py::arg("halflife_s"))
        .def("update", &EwmaVariance::update, py::arg("mid"), py::arg("ts_ns"))
        .def("reset", &EwmaVariance::reset)
        .def("value", &EwmaVariance::value)
        .def("count", &EwmaVariance::count)
        .def("last_mid", &EwmaVariance::last_mid)
        .def("last_ns", &EwmaVariance::last_ns);

    py::class_<EwmaDrift>(m, "EwmaDrift", "EWMA of signed time-normalised log returns — per-√second drift µ.")
        .def(py::init<double>(), py::arg("halflife_s"))
        .def("update", &EwmaDrift::update, py::arg("mid"), py::arg("ts_ns"))
        .def("reset", &EwmaDrift::reset)
        .def("value", &EwmaDrift::value)
        .def("count", &EwmaDrift::count)
        .def("last_mid", &EwmaDrift::last_mid)
        .def("last_ns", &EwmaDrift::last_ns);

    py::class_<KappaEstimator>(m,
                               "KappaEstimator",
                               "EWMA of per-side trade-arrival rate κ (trades/s). "
                               "Observation each trade: 0.5/dt_s (splits across bid/ask).")
        .def(py::init<double>(), py::arg("halflife_s"))
        .def("update", &KappaEstimator::update, py::arg("trade_ts_ns"))
        .def("reset", &KappaEstimator::reset)
        .def("value", &KappaEstimator::value)
        .def("count", &KappaEstimator::count)
        .def("last_trade_ns", &KappaEstimator::last_trade_ns);

    // ─── Hurst free functions ─────────────────────────────────────────────
    // Accept Python lists / numpy arrays via pybind11::stl auto-conversion.
    m.def(
        "compute_hurst",
        [](const std::vector<double>& returns, std::size_t max_window) {
            return compute_hurst(returns.data(), returns.size(), max_window);
        },
        py::arg("returns"),
        py::arg("max_window"),
        "Single-window rescaled-range Hurst over the first len(returns) entries.");

    m.def(
        "compute_hurst_multi_window",
        [](const std::vector<double>& returns, std::size_t max_window) {
            return compute_hurst_multi_window(returns.data(), returns.size(), max_window);
        },
        py::arg("returns"),
        py::arg("max_window"),
        "Multi-window R/S Hurst (regression slope) — more robust on short series.");

    // ─── RegimeDetector (Hurst-based) ─────────────────────────────────────
    py::class_<RegimeDetector> rd(m,
                                  "RegimeDetector",
                                  "Rolling Hurst-based regime classifier: MEAN_REVERT / NEUTRAL / TRENDING.");

    py::enum_<RegimeDetector::Regime>(rd, "Regime")
        .value("WARMING_UP", RegimeDetector::Regime::WARMING_UP)
        .value("MEAN_REVERT", RegimeDetector::Regime::MEAN_REVERT)
        .value("NEUTRAL", RegimeDetector::Regime::NEUTRAL)
        .value("TRENDING", RegimeDetector::Regime::TRENDING);

    py::class_<RegimeDetector::Config>(rd, "Config")
        .def(py::init<>())
        .def_readwrite("mean_revert_threshold", &RegimeDetector::Config::mean_revert_threshold)
        .def_readwrite("trend_threshold", &RegimeDetector::Config::trend_threshold)
        .def_readwrite("hysteresis", &RegimeDetector::Config::hysteresis)
        .def_readwrite("hurst_window", &RegimeDetector::Config::hurst_window)
        .def_readwrite("warmup_samples", &RegimeDetector::Config::warmup_samples)
        .def_readwrite("gamma_mult_mean_revert", &RegimeDetector::Config::gamma_mult_mean_revert)
        .def_readwrite("gamma_mult_neutral", &RegimeDetector::Config::gamma_mult_neutral)
        .def_readwrite("gamma_mult_trending", &RegimeDetector::Config::gamma_mult_trending)
        .def_readwrite("eval_interval", &RegimeDetector::Config::eval_interval);

    rd.def(py::init<>())
        .def(py::init<RegimeDetector::Config>(), py::arg("config"))
        .def("update", &RegimeDetector::update, py::arg("mid"))
        .def("regime", &RegimeDetector::regime)
        .def("regime_name", &RegimeDetector::regime_name)
        .def("hurst", &RegimeDetector::hurst)
        .def("gamma_multiplier", &RegimeDetector::gamma_multiplier)
        .def("is_warm", &RegimeDetector::is_warm)
        .def("tick_count", &RegimeDetector::tick_count);

    // ─── RegimeClassifier (vol + trend z-score) ───────────────────────────
    py::class_<RegimeClassifier> rc(m,
                                    "RegimeClassifier",
                                    "Vol + trend-z-score binary regime gate: QUIET / TRENDING / CHOPPY.");

    py::enum_<RegimeClassifier::Regime>(rc, "Regime")
        .value("QUIET", RegimeClassifier::Regime::QUIET)
        .value("TRENDING", RegimeClassifier::Regime::TRENDING)
        .value("CHOPPY", RegimeClassifier::Regime::CHOPPY);

    py::class_<RegimeClassifier::Config>(rc, "Config")
        .def(py::init<>())
        .def_readwrite("window_size", &RegimeClassifier::Config::window_size)
        .def_readwrite("sample_interval_ns", &RegimeClassifier::Config::sample_interval_ns)
        .def_readwrite("quiet_vol_bps_per_min", &RegimeClassifier::Config::quiet_vol_bps_per_min)
        .def_readwrite("trend_threshold_z", &RegimeClassifier::Config::trend_threshold_z)
        .def_readwrite("chop_cooldown_ns", &RegimeClassifier::Config::chop_cooldown_ns);

    rc.def(py::init<>())
        .def(py::init<RegimeClassifier::Config>(), py::arg("config"))
        .def("update",
             &RegimeClassifier::update,
             py::arg("mid"),
             py::arg("ts_ns"),
             "Feed a mid-price observation. Returns ready() after the update.")
        .def("ready", &RegimeClassifier::ready)
        .def("realized_vol_bps_per_min", &RegimeClassifier::realized_vol_bps_per_min)
        .def("trend_zscore", &RegimeClassifier::trend_zscore)
        .def("classify", &RegimeClassifier::classify, py::arg("now_ns"))
        .def("reset", &RegimeClassifier::reset);

    // ─── Fill probability models (free functions) ─────────────────────────
    m.def("fill_probability_poisson",
          &bpt::features::fill_probability_poisson,
          py::arg("kappa"),
          py::arg("horizon_s"),
          py::arg("queue_ahead"),
          "Poisson-arrival fill probability over horizon_s. "
          "P ≈ 1 - exp(-kappa * horizon_s / queue_ahead). "
          "Returns 1 when queue_ahead<=0; 0 when kappa<=0 or horizon<=0.");
    m.def("fill_probability_geometric",
          &bpt::features::fill_probability_geometric,
          py::arg("our_qty"),
          py::arg("queue_ahead"),
          "Ordinal queue-share fallback: our_qty / (our_qty + queue_ahead). "
          "Not a true probability; useful as a unit-free ranking signal.");
}
