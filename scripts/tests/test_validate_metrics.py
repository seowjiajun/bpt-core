"""KS distance + scalar diff sanity tests."""

import math
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from validate_lib.metrics import (  # noqa: E402
    distribution_diff,
    ks_distance,
    relative_count_diff,
)


def test_ks_zero_for_identical_distributions():
    a = [1.0, 2.0, 3.0, 4.0, 5.0]
    b = list(a)
    assert ks_distance(a, b) == 0.0


def test_ks_one_for_disjoint_distributions():
    a = [1.0, 2.0, 3.0]
    b = [10.0, 20.0, 30.0]
    # Walking the union: at x=3 → cdf_a=1.0, cdf_b=0.0 → gap=1.0.
    assert ks_distance(a, b) == 1.0


def test_ks_handles_duplicate_values():
    # Both distributions have identical multisets — ks must be 0 even with
    # repeats (regression: a naive walk that skips one side at a tie can
    # produce spurious gaps).
    a = [1.0, 1.0, 2.0, 2.0, 3.0]
    b = [1.0, 1.0, 2.0, 2.0, 3.0]
    assert ks_distance(a, b) == 0.0


def test_ks_known_shifted_distribution():
    # a = [0, 1, 2, 3], b = [1, 2, 3, 4]. At x=0: cdf_a=0.25, cdf_b=0 → gap 0.25.
    # That's the max gap because subsequent walks close it.
    a = [0.0, 1.0, 2.0, 3.0]
    b = [1.0, 2.0, 3.0, 4.0]
    assert ks_distance(a, b) == 0.25


def test_ks_empty_inputs():
    assert ks_distance([], []) == 0.0
    assert ks_distance([1.0], []) == 1.0
    assert ks_distance([], [1.0]) == 1.0


def test_distribution_diff_summary_stats():
    d = distribution_diff([1.0, 2.0, 3.0], [10.0, 20.0, 30.0])
    assert d.sim_n == 3
    assert d.prod_n == 3
    assert d.sim_mean == 2.0
    assert d.prod_mean == 20.0
    assert d.ks > 0.5  # disjoint enough


def test_relative_count_diff_basic():
    assert relative_count_diff(110, 100) == 0.10
    assert relative_count_diff(90, 100) == -0.10
    assert relative_count_diff(0, 0) == 0.0


def test_relative_count_diff_zero_prod():
    assert relative_count_diff(5, 0) == math.inf
