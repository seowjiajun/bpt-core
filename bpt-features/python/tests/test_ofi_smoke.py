"""Smoke tests: prove the pybind11 bindings load + each class round-trips.

Intentionally minimal — catches "binding broken at compile time", "Python
can't find the .so", or "constructor/method signature drifted." Full
numerical correctness is covered by the C++ tests (features_unit_tests).

Filename kept as test_ofi_smoke.py for git-blame continuity; covers
all 4 bound feature classes today.
"""

import unittest

import bpt_features as bf


class TestExportsPresent(unittest.TestCase):
    def test_module_exports(self):
        # Raw C++ classes
        for cls in ["OFICalculator", "FairValueEstimator",
                    "RealizedVolEstimator", "VolatilityGate"]:
            self.assertTrue(hasattr(bf, cls), f"missing class: {cls}")
        # Function wrappers
        for fn in ["ofi", "microprice", "mid_price", "fair_value_ewma",
                   "microprice_size_capped", "realized_vol", "vol_gate"]:
            self.assertTrue(hasattr(bf, fn), f"missing wrapper: {fn}")


class TestOFISmoke(unittest.TestCase):
    def test_construct_and_update(self):
        cfg = bf.OFICalculator.Config()
        cfg.max_levels = 5
        cfg.window_ns = 1_000_000_000

        calc = bf.OFICalculator(cfg)
        self.assertFalse(calc.is_warm())

        v = calc.update(bids=[(100.0, 10.0)], asks=[(100.1, 8.0)],
                        timestamp_ns=1_000_000_000)
        self.assertEqual(v, 0.0)  # first call, no diff

        v2 = calc.update(bids=[(100.0, 15.0)], asks=[(100.1, 5.0)],
                         timestamp_ns=1_100_000_000)
        self.assertTrue(calc.is_warm())
        self.assertGreater(v2, 0.0)  # buy-pressure → positive OFI


class TestFairValueSmoke(unittest.TestCase):
    def test_microprice_basic(self):
        cfg = bf.FairValueEstimator.Config()
        cfg.mode = bf.FairValueEstimator.Mode.Micro
        fve = bf.FairValueEstimator(cfg)
        # Symmetric book → microprice = mid
        v = fve.estimate(bid_px=100.0, ask_px=100.2, bid_qty=10.0, ask_qty=10.0)
        self.assertAlmostEqual(v, 100.1, places=6)

    def test_microprice_tilts_to_thin_side(self):
        cfg = bf.FairValueEstimator.Config()
        cfg.mode = bf.FairValueEstimator.Mode.Micro
        fve = bf.FairValueEstimator(cfg)
        # Big bid, small ask → micro tilts toward ask (ask is thinner)
        v = fve.estimate(bid_px=100.0, ask_px=100.2, bid_qty=100.0, ask_qty=1.0)
        self.assertGreater(v, 100.1)

    def test_mid_ignores_qty(self):
        cfg = bf.FairValueEstimator.Config()
        cfg.mode = bf.FairValueEstimator.Mode.Mid
        fve = bf.FairValueEstimator(cfg)
        v = fve.estimate(bid_px=100.0, ask_px=100.2, bid_qty=100.0, ask_qty=1.0)
        self.assertAlmostEqual(v, 100.1, places=6)


class TestRealizedVolSmoke(unittest.TestCase):
    def test_construct_update_compute(self):
        # 10-sample window, 100ms sample interval
        est = bf.RealizedVolEstimator(window_size=10, sample_interval_ns=100_000_000)
        self.assertEqual(est.count(), 0)
        self.assertFalse(est.ready())

        # Feed 15 ticks 100ms apart with varying mid
        for i in range(15):
            est.update(100.0 + 0.01 * i, 100_000_000 * (i + 1))
        self.assertTrue(est.ready())
        self.assertGreater(est.realized_vol(), 0.0)

    def test_reset_clears(self):
        est = bf.RealizedVolEstimator(window_size=5, sample_interval_ns=100_000_000)
        for i in range(10):
            est.update(100.0, 100_000_000 * (i + 1))
        est.reset()
        self.assertEqual(est.count(), 0)


class TestVolGateSmoke(unittest.TestCase):
    def test_disabled_when_zero(self):
        cfg = bf.VolatilityGate.Config()
        cfg.max_bps_per_window = 0.0
        gate = bf.VolatilityGate(cfg)
        self.assertFalse(gate.enabled())
        # Even a large move shouldn't halt when disabled
        self.assertFalse(gate.update_and_check(100.0, 1_000_000_000))
        self.assertFalse(gate.update_and_check(150.0, 1_010_000_000))

    def test_trips_on_large_move(self):
        cfg = bf.VolatilityGate.Config()
        cfg.max_bps_per_window = 50.0
        gate = bf.VolatilityGate(cfg)
        self.assertTrue(gate.enabled())

        # Quiet tick — should not trip
        self.assertFalse(gate.update_and_check(100.0, 1_000_000_000))
        # 100 bps jump (over 50 threshold) — should trip
        halted = gate.update_and_check(101.0, 1_010_000_000)
        self.assertTrue(halted)
        self.assertGreater(gate.last_trip_bps(), 50.0)


if __name__ == "__main__":
    unittest.main()
