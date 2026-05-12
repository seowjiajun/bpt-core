import pytest

from bpt_ops.common.schema import InstrumentMapping, ReverseEntry
from bpt_ops.jobs.instrument_mapping import canonical_ids


def _empty() -> InstrumentMapping:
    return InstrumentMapping(forward={}, reverse={}, exported_at=0, instrument_count=0)


def _with(entries: dict[str, ReverseEntry]) -> InstrumentMapping:
    return InstrumentMapping(
        forward={}, reverse=entries, exported_at=0, instrument_count=len(entries)
    )


def test_next_id_starts_at_range_start():
    assert canonical_ids.next_canonical_id(_empty(), "PERP") == 1001
    assert canonical_ids.next_canonical_id(_empty(), "SPOT") == 2001
    assert canonical_ids.next_canonical_id(_empty(), "FUTURES") == 3001


def test_next_id_increments_past_max_in_range():
    m = _with(
        {
            "1001": ReverseEntry(base="BTC", quote="USDT", type="PERP", exchanges={}),
            "1005": ReverseEntry(base="ETH", quote="USDT", type="PERP", exchanges={}),
        }
    )
    assert canonical_ids.next_canonical_id(m, "PERP") == 1006


def test_next_id_ignores_other_type_buckets():
    m = _with(
        {
            "2001": ReverseEntry(base="BTC", quote="USDT", type="SPOT", exchanges={}),
            "2002": ReverseEntry(base="ETH", quote="USDT", type="SPOT", exchanges={}),
        }
    )
    assert canonical_ids.next_canonical_id(m, "PERP") == 1001


def test_find_by_triple():
    m = _with(
        {
            "2001": ReverseEntry(base="BTC", quote="USDT", type="SPOT", exchanges={}),
        }
    )
    assert canonical_ids.find_canonical_id(m, "BTC", "USDT", "SPOT") == 2001
    assert canonical_ids.find_canonical_id(m, "BTC", "USDT", "PERP") is None


def test_assign_reuses_existing():
    m = _with(
        {
            "2001": ReverseEntry(base="BTC", quote="USDT", type="SPOT", exchanges={}),
        }
    )
    assert canonical_ids.assign_canonical_id(m, "BTC", "USDT", "SPOT") == 2001


def test_assign_allocates_new():
    m = _with(
        {
            "2001": ReverseEntry(base="BTC", quote="USDT", type="SPOT", exchanges={}),
        }
    )
    assert canonical_ids.assign_canonical_id(m, "ETH", "USDT", "SPOT") == 2002


def test_range_exhaustion_raises():
    # Fill the PERP range from top
    entries = {
        str(1999): ReverseEntry(base="X", quote="USDT", type="PERP", exchanges={}),
    }
    m = _with(entries)
    with pytest.raises(RuntimeError, match="range exhausted"):
        canonical_ids.next_canonical_id(m, "PERP")
