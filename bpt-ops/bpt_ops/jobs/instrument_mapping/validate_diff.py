"""Sanity-check a proposed instrument-mapping diff before it merges.

Run by CI on every PR that touches config/instruments/. Catches:
  - Mass deletions: > MAX_DELETE_PCT of an exchange's instruments removed
    without an explicit override label on the PR (saves you from a bad
    API scrape wiping the universe)
  - Shape corruption: the new file doesn't conform to InstrumentMapping's
    pydantic schema (instrument_count mismatch, unknown type, etc.)
  - Canonical-id churn: an existing canonical_id now points at a different
    (base, quote, type) triple (strategies cache IDs — renumbering them
    silently would be very bad)

Exits 0 when the diff passes, non-zero with a human-readable explanation
when it doesn't. When non-zero, CI fails; the PR stays open for review.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from pydantic import ValidationError

from bpt_ops.common.schema import InstrumentMapping

# Reject a PR that removes > this fraction of an exchange's instruments unless
# the `mapping-override` label is attached. Tune as the universe grows — 0.2
# is conservative for the ~6-580 instrument range we're working with.
MAX_DELETE_PCT = 0.2


class ValidationFailure(Exception):
    pass


def _load(path: Path) -> InstrumentMapping:
    raw = json.loads(path.read_text())
    return InstrumentMapping.model_validate(raw)


def _check_mass_delete(
    *, exchange: str, prev: InstrumentMapping, new: InstrumentMapping, override: bool
) -> None:
    prev_ids = set(prev.reverse.keys())
    new_ids = set(new.reverse.keys())
    removed = prev_ids - new_ids
    if not prev_ids:
        return
    pct = len(removed) / len(prev_ids)
    if pct > MAX_DELETE_PCT and not override:
        raise ValidationFailure(
            f"{exchange}: {len(removed)}/{len(prev_ids)} instruments removed "
            f"({pct:.1%} > threshold {MAX_DELETE_PCT:.0%}). Attach the "
            f"`mapping-override` label to force-merge if this is intended."
        )


def _check_id_stability(*, exchange: str, prev: InstrumentMapping, new: InstrumentMapping) -> None:
    for cid, prev_entry in prev.reverse.items():
        new_entry = new.reverse.get(cid)
        if new_entry is None:
            continue
        if (
            new_entry.base != prev_entry.base
            or new_entry.quote != prev_entry.quote
            or new_entry.type != prev_entry.type
        ):
            raise ValidationFailure(
                f"{exchange}: canonical_id {cid} changed meaning — "
                f"was {prev_entry.base}/{prev_entry.quote} {prev_entry.type}, "
                f"now {new_entry.base}/{new_entry.quote} {new_entry.type}. "
                "Renumbering existing IDs is never allowed."
            )


def validate(previous_dir: Path, new_dir: Path, *, override: bool) -> list[str]:
    """Diff two directories of instrument_mapping.*.json. Returns only hard
    failures — empty list means the PR is safe to merge. Informational notes
    (new exchange added, etc.) are printed but not returned."""
    failures: list[str] = []

    new_files = sorted(new_dir.glob("instrument_mapping.*.json"))
    for new_path in new_files:
        name = new_path.name
        prev_path = previous_dir / name

        try:
            new_mapping = _load(new_path)
        except ValidationError as e:
            failures.append(f"{name}: schema violation — {e}")
            continue

        if not prev_path.exists():
            print(f"ℹ  {name}: new exchange added; no baseline to diff against")
            continue

        try:
            prev_mapping = _load(prev_path)
        except ValidationError as e:
            failures.append(f"{name}: baseline schema violation (unusual) — {e}")
            continue

        exchange = name.removeprefix("instrument_mapping.").removesuffix(".json")
        try:
            _check_mass_delete(
                exchange=exchange, prev=prev_mapping, new=new_mapping, override=override
            )
            _check_id_stability(exchange=exchange, prev=prev_mapping, new=new_mapping)
        except ValidationFailure as e:
            failures.append(str(e))

    return failures


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--previous", required=True, type=Path, help="baseline mapping dir (from main)")
    p.add_argument("--new", required=True, type=Path, help="PR's proposed mapping dir")
    p.add_argument(
        "--override",
        action="store_true",
        help="skip mass-delete threshold (wire to the `mapping-override` PR label)",
    )
    args = p.parse_args()

    findings = validate(args.previous, args.new, override=args.override)
    if findings:
        print("❌ Mapping diff validation failed:", file=sys.stderr)
        for f in findings:
            print(f"   - {f}", file=sys.stderr)
        return 1
    print("✓ Mapping diff passes all checks.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
