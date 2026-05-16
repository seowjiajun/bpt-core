"""Walk-forward train/test window splitter.

Given a contiguous date range and split sizes, produces a list of
(train_window, test_window) pairs the sweep runner can feed into
[[simulation.windows]] in the strategy config. Intentionally pure — no
filesystem, no subprocess, no clock dependency — so behaviour is testable
in isolation.

Conventions:
- Times are UTC. Inputs accept either "YYYY-MM-DD" (= midnight UTC) or
  full ISO 8601 ("...THH:MM:SSZ"). Outputs are always full ISO 8601 so
  downstream config consumers don't have to special-case the date-only form.
- Half-open windows: [start, end). Matches the DataLoader contract.
- Purge gap is inserted *between* train end and test start. It also
  applies between test end and the next split's train start, so adjacent
  splits don't share information through any cache or stateful component.
"""

from __future__ import annotations

import datetime as dt
from dataclasses import dataclass


@dataclass(frozen=True)
class Window:
    start: str  # ISO 8601, e.g. "2026-05-08T00:00:00Z"
    end: str  # ISO 8601, exclusive

    def to_toml(self) -> dict:
        return {"start": self.start, "end": self.end}


@dataclass(frozen=True)
class Split:
    train: Window
    test: Window
    index: int  # 0-based ordinal


def _parse(s: str) -> dt.datetime:
    """Accept 'YYYY-MM-DD' (midnight UTC) or '...THH:MM:SSZ' (any HMS)."""
    if len(s) == 10:
        return dt.datetime.strptime(s, "%Y-%m-%d").replace(tzinfo=dt.timezone.utc)
    if s.endswith("Z"):
        return dt.datetime.fromisoformat(s[:-1]).replace(tzinfo=dt.timezone.utc)
    raise ValueError(f"unrecognised timestamp form: {s!r}")


def _format(t: dt.datetime) -> str:
    """Always emit ...THH:MM:SSZ — DataLoader prefers the full form."""
    return t.astimezone(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def make_splits(
    start: str,
    end: str,
    train_days: int,
    test_days: int,
    step_days: int = 1,
    purge_minutes: int = 0,
) -> list[Split]:
    """Generate (train, test) splits over the [start, end) range.

    Each split is anchored by `train_start`:
      train_start ─train_days─▶ train_end ─purge─▶ test_start ─test_days─▶ test_end
    The next split's train_start = previous train_start + step_days.

    A split is included only if test_end ≤ end. Edge cases:
    - A start equal to end produces zero splits.
    - train_days/test_days/step_days must each be ≥ 1.
    - purge_minutes ≥ 0; 0 means train_end == test_start.

    Returns splits in chronological order with stable index attribute.
    """
    if train_days < 1 or test_days < 1 or step_days < 1:
        raise ValueError("train_days, test_days, step_days must each be ≥ 1")
    if purge_minutes < 0:
        raise ValueError("purge_minutes must be ≥ 0")

    range_start = _parse(start)
    range_end = _parse(end)
    if range_end <= range_start:
        raise ValueError(f"end ({end}) must be strictly after start ({start})")

    train_dur = dt.timedelta(days=train_days)
    test_dur = dt.timedelta(days=test_days)
    step_dur = dt.timedelta(days=step_days)
    purge = dt.timedelta(minutes=purge_minutes)

    splits: list[Split] = []
    train_start = range_start
    i = 0
    while True:
        train_end = train_start + train_dur
        test_start = train_end + purge
        test_end = test_start + test_dur
        if test_end > range_end:
            break
        splits.append(
            Split(
                train=Window(_format(train_start), _format(train_end)),
                test=Window(_format(test_start), _format(test_end)),
                index=i,
            )
        )
        train_start += step_dur
        i += 1
    return splits
