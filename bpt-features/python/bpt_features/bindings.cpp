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

#include "features/ofi_calculator.h"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>  // for std::vector ↔ Python list conversion

namespace py = pybind11;
using bpt::features::OFICalculator;

PYBIND11_MODULE(_core, m) {
    m.doc() = "bpt-features C++ extension — compiled from bpt-features/python/bindings.cpp";

    // ─────────────────────────── OFICalculator ───────────────────────────
    // Cont-Kukanov-Stoikov rolling Order-Flow Imbalance. See
    // features/ofi_calculator.h for the math + per-level weighting rules.
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
}
