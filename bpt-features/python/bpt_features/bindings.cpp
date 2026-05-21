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

#include "features/fair_value.h"
#include "features/ofi.h"
#include "features/realized_vol.h"
#include "features/vol_gate.h"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>  // for std::vector ↔ Python list conversion

namespace py = pybind11;
using bpt::features::FairValueEstimator;
using bpt::features::OFICalculator;
using bpt::features::RealizedVolEstimator;
using bpt::features::VolatilityGate;

PYBIND11_MODULE(_core, m) {
    m.doc() = "bpt-features C++ extension — compiled from bpt-features/python/bindings.cpp";

    // ─────────────────────────── OFICalculator ───────────────────────────
    // Cont-Kukanov-Stoikov rolling Order-Flow Imbalance. See
    // features/ofi.h for the math + per-level weighting rules.
    py::class_<OFICalculator> ofi(m, "OFICalculator",
                                  "Rolling multi-level Order-Flow Imbalance estimator");

    py::class_<OFICalculator::Config>(ofi, "Config")
        .def(py::init<>())
        .def_readwrite("max_levels", &OFICalculator::Config::max_levels)
        .def_readwrite("window_ns", &OFICalculator::Config::window_ns);

    ofi.def(py::init<OFICalculator::Config>(), py::arg("cfg"))
        // update() is the hot path. The (price, qty) Level pairs are
        // std::pair<double, double>; pybind11/stl.h auto-converts a
        // Python list-of-tuples into the C++ vector representation.
        .def("update", &OFICalculator::update, py::arg("bids"), py::arg("asks"),
             py::arg("timestamp_ns"), "Feed a book snapshot, return the current rolling OFI value")
        .def("is_warm", &OFICalculator::is_warm,
             "True once at least two updates have produced a value")
        .def("value", &OFICalculator::value,
             "Last value returned by update(); 0 if never called")
        .def("avg_depth", &OFICalculator::avg_depth,
             "Rolling average top-K depth (bid+ask) over the window")
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
    py::class_<FairValueEstimator> fve(m, "FairValueEstimator",
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
             py::arg("bid_px"), py::arg("ask_px"), py::arg("bid_qty"), py::arg("ask_qty"),
             "Compute fair value from top-of-book. Returns NaN on degenerate quote.")
        .def("last_estimate", &FairValueEstimator::last_estimate,
             "Last computed value; NaN if estimate() never called or book was unready")
        .def("mode", &FairValueEstimator::mode, "Current mode");

    // ───────────────────────── RealizedVolEstimator ──────────────────────
    // Rolling-window annualised realised vol from mid-price returns.
    // Sampled at fixed intervals (sample_interval_ns) so the rate is
    // deterministic regardless of tick density.
    py::class_<RealizedVolEstimator>(m, "RealizedVolEstimator",
                                     "Rolling annualised realised vol estimator")
        .def(py::init<size_t, uint64_t>(),
             py::arg("window_size"), py::arg("sample_interval_ns"),
             "window_size: number of samples in the rolling window. "
             "sample_interval_ns: minimum spacing between samples (ticks closer than "
             "this are ignored).")
        .def("update", &RealizedVolEstimator::update,
             py::arg("mid_price"), py::arg("timestamp_ns"),
             "Feed a mid-price tick. Returns True if this tick was accepted as a sample "
             "(i.e. enough time has elapsed since the last accepted sample).")
        .def("realized_vol", &RealizedVolEstimator::realized_vol,
             "Current realised vol (annualised, in price units). 0 until ready().")
        .def("count", &RealizedVolEstimator::count,
             "Number of samples currently in the rolling window.")
        .def("ready", &RealizedVolEstimator::ready,
             "True once min_samples samples have been collected.")
        .def("reset", &RealizedVolEstimator::reset, "Reset all internal state.");

    // ─────────────────────────── VolatilityGate ──────────────────────────
    // Suppression gate that trips on a short-horizon mid-price move
    // exceeding `max_bps_per_window`. While tripped, is_halted() returns
    // true and the strategy should suppress quoting.
    py::class_<VolatilityGate> volgate(m, "VolatilityGate",
                                       "Vol-spike suppression gate (trips on rapid mid moves)");

    py::class_<VolatilityGate::Config>(volgate, "Config")
        .def(py::init<>())
        .def_readwrite("max_bps_per_window", &VolatilityGate::Config::max_bps_per_window);

    volgate.def(py::init<VolatilityGate::Config>(), py::arg("cfg"))
        .def("update_and_check", &VolatilityGate::update_and_check,
             py::arg("mid"), py::arg("now_ns"),
             "Feed a mid-price tick. Returns True if the gate is currently HALTED.")
        .def("is_halted", &VolatilityGate::is_halted, py::arg("now_ns"),
             "True if the gate is currently in its post-trip halt window.")
        .def("last_trip_bps", &VolatilityGate::last_trip_bps,
             "Bps move that triggered the most recent trip (0 if never tripped).")
        .def("enabled", &VolatilityGate::enabled,
             "True if max_bps_per_window > 0 (gate is configured to fire).")
        .def("max_bps_per_window", &VolatilityGate::max_bps_per_window)
        .def("set_max_bps_per_window", &VolatilityGate::set_max_bps_per_window,
             py::arg("max_bps"));
}
