"""Smoke test: prove the pybind11 binding loads + the class works.

This is intentionally minimal — just enough to catch "binding broken
at compile time" or "Python can't find the .so". Full correctness is
covered by the C++ test (features_unit_tests).
"""

import unittest

import bpt_features as bf


class TestOFISmoke(unittest.TestCase):
    def test_module_imports(self):
        """bf.OFICalculator + bf.ofi are accessible."""
        self.assertTrue(hasattr(bf, "OFICalculator"))
        self.assertTrue(hasattr(bf, "ofi"))

    def test_construct_and_update(self):
        """Build an OFICalculator, feed one quote, get a number out."""
        cfg = bf.OFICalculator.Config()
        cfg.max_levels = 5
        cfg.window_ns = 1_000_000_000

        calc = bf.OFICalculator(cfg)
        self.assertFalse(calc.is_warm())

        # First update — no previous snapshot to diff against, returns 0.
        v = calc.update(
            bids=[(100.0, 10.0), (99.9, 20.0)],
            asks=[(100.1, 8.0), (100.2, 15.0)],
            timestamp_ns=1_000_000_000,
        )
        self.assertEqual(v, 0.0)

        # Second update — buy pressure (bid grew, ask shrank).
        v2 = calc.update(
            bids=[(100.0, 15.0), (99.9, 20.0)],  # bid qty grew 10 → 15
            asks=[(100.1, 5.0), (100.2, 15.0)],  # ask qty shrunk 8 → 5
            timestamp_ns=1_100_000_000,
        )
        self.assertTrue(calc.is_warm())
        # Net buy pressure → positive OFI.
        self.assertGreater(v2, 0.0)

    def test_reset_clears_state(self):
        cfg = bf.OFICalculator.Config()
        calc = bf.OFICalculator(cfg)
        calc.update([(100.0, 5.0)], [(100.1, 5.0)], 1_000_000_000)
        calc.update([(100.0, 10.0)], [(100.1, 5.0)], 1_100_000_000)
        self.assertTrue(calc.is_warm())

        calc.reset()
        self.assertFalse(calc.is_warm())
        self.assertEqual(calc.value(), 0.0)


if __name__ == "__main__":
    unittest.main()
