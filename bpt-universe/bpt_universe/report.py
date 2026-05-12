from __future__ import annotations

from datetime import UTC, datetime

import polars as pl


def _fmt(v, spec=".4f"):
    if v is None:
        return "—"
    try:
        return format(v, spec)
    except (TypeError, ValueError):
        return str(v)


def render(
    candidates: pl.DataFrame,
    to_add: list[str],
    to_remove: list[str],
    diff_text: str,
    top_n: int = 30,
) -> str:
    """Markdown report for the operator to skim before committing the diff.

    Layout: summary → diff → top-N table with the features that fed the score.
    Tables are intentionally narrow so they render in PR previews.
    """
    feat_cols = [
        c
        for c in (
            "samples",
            "spread_bps_p50",
            "spread_bps_p95",
            "depth_top_mean",
            "fills",
            "fee_bps_mean",
            "markout_bps_mean",
            "realized_capture_bps",
            "tick_floor_bps",
        )
        if c in candidates.columns
    ]

    ts = datetime.now(UTC).strftime("%Y-%m-%d %H:%M UTC")
    n = candidates.height
    parts = [
        f"# bpt-universe — candidate review ({ts})",
        "",
        f"- Candidates passing filters: **{n}**",
        f"- Adds proposed: **{len(to_add)}**",
        f"- Removes proposed: **{len(to_remove)}**",
        "",
        "## Proposed config diff",
        "",
        "```diff",
        diff_text.rstrip(),
        "```",
        "",
    ]

    show = candidates.head(top_n)
    if show.is_empty():
        parts.append("_No candidates after filters._")
        return "\n".join(parts) + "\n"

    parts.append(f"## Top {min(top_n, n)} candidates")
    parts.append("")
    headers = ["#", "exchange", "symbol", "inst_type", "score", *feat_cols]
    parts.append("| " + " | ".join(headers) + " |")
    parts.append("|" + "|".join(["---"] * len(headers)) + "|")
    for i, row in enumerate(show.iter_rows(named=True), start=1):
        cells = [
            str(i),
            row.get("exchange", "—") or "—",
            row.get("symbol", "—") or "—",
            row.get("inst_type", "—") or "—",
            _fmt(row.get("score")),
        ]
        for c in feat_cols:
            cells.append(_fmt(row.get(c)))
        parts.append("| " + " | ".join(cells) + " |")
    parts.append("")
    return "\n".join(parts) + "\n"
