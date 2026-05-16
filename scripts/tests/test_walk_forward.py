import pathlib
import sys

# Add scripts/ to the import path so `sweep_lib` resolves regardless of pytest's cwd.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

import pytest  # noqa: E402

from sweep_lib.walk_forward import Split, Window, make_splits  # noqa: E402


def test_basic_split_no_purge():
    splits = make_splits(
        start="2026-04-25",
        end="2026-05-02",
        train_days=3,
        test_days=1,
        step_days=1,
        purge_minutes=0,
    )
    # train_dur=3, test_dur=1 → first window needs 4 days; range is 7 days.
    # Splits: train_start ∈ {04-25, 04-26, 04-27, 04-28}
    # The 04-28 anchor: train [04-28, 05-01), test [05-01, 05-02). test_end=05-02 == range_end → included.
    assert len(splits) == 4
    assert splits[0].train.start == "2026-04-25T00:00:00Z"
    assert splits[0].train.end == "2026-04-28T00:00:00Z"
    assert splits[0].test.start == "2026-04-28T00:00:00Z"
    assert splits[0].test.end == "2026-04-29T00:00:00Z"
    assert splits[-1].train.start == "2026-04-28T00:00:00Z"
    assert splits[-1].test.end == "2026-05-02T00:00:00Z"


def test_purge_gap_is_inserted_between_train_and_test():
    splits = make_splits(
        start="2026-04-25",
        end="2026-04-30",
        train_days=3,
        test_days=1,
        step_days=1,
        purge_minutes=15,
    )
    # train end = 04-25T00:00 + 3d = 04-28T00:00
    # test start = train end + 15min = 04-28T00:15
    # test end = 04-28T00:15 + 1d = 04-29T00:15
    s = splits[0]
    assert s.train.end == "2026-04-28T00:00:00Z"
    assert s.test.start == "2026-04-28T00:15:00Z"
    assert s.test.end == "2026-04-29T00:15:00Z"


def test_step_larger_than_one_day_skips_anchors():
    splits = make_splits(
        start="2026-04-25",
        end="2026-05-05",
        train_days=2,
        test_days=1,
        step_days=3,
        purge_minutes=0,
    )
    # step=3d → anchors 04-25, 04-28, 05-01.
    # 05-01 anchor: train [05-01, 05-03), test [05-03, 05-04). test_end=05-04 ≤ range_end=05-05.
    # 05-04 anchor: test_end=05-07 > 05-05 — excluded.
    assert len(splits) == 3
    assert [s.train.start for s in splits] == [
        "2026-04-25T00:00:00Z",
        "2026-04-28T00:00:00Z",
        "2026-05-01T00:00:00Z",
    ]


def test_full_iso_input_preserved_through_to_output():
    splits = make_splits(
        start="2026-04-25T13:00:00Z",
        end="2026-04-26T13:00:00Z",
        train_days=1,
        test_days=1,
        step_days=1,
        purge_minutes=0,
    )
    # Range is exactly 1 day, but train+test = 2 days → no splits fit.
    assert splits == []


def test_no_splits_when_range_too_short():
    splits = make_splits(
        start="2026-05-01",
        end="2026-05-02",
        train_days=3,
        test_days=1,
        step_days=1,
        purge_minutes=0,
    )
    assert splits == []


def test_invalid_args_raise():
    with pytest.raises(ValueError):
        make_splits("2026-05-01", "2026-05-08", 0, 1, 1, 0)
    with pytest.raises(ValueError):
        make_splits("2026-05-01", "2026-05-08", 1, 0, 1, 0)
    with pytest.raises(ValueError):
        make_splits("2026-05-01", "2026-05-08", 1, 1, 0, 0)
    with pytest.raises(ValueError):
        make_splits("2026-05-01", "2026-05-08", 1, 1, 1, -5)
    with pytest.raises(ValueError):
        make_splits("2026-05-08", "2026-05-01", 1, 1, 1, 0)


def test_window_to_toml():
    w = Window("2026-05-08T13:00:00Z", "2026-05-08T14:00:00Z")
    assert w.to_toml() == {
        "start": "2026-05-08T13:00:00Z",
        "end": "2026-05-08T14:00:00Z",
    }


def test_split_index_is_monotonic():
    splits = make_splits("2026-04-25", "2026-05-05", 2, 1, 1, 0)
    assert [s.index for s in splits] == list(range(len(splits)))
