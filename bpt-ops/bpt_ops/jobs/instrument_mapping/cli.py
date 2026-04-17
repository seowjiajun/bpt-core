"""`bpt-ops instrument-mapping` subcommand.

Fetches per-exchange instruments, reconciles into canonical IDs, writes
one JSON file per exchange to --output-dir.
"""
from __future__ import annotations

from pathlib import Path

import typer

from bpt_ops.common.schema import ExchangeId
from bpt_ops.jobs.instrument_mapping import emit, reconcile
from bpt_ops.jobs.instrument_mapping.fetchers.base import RawInstrument, fetch_for

app = typer.Typer(
    help="Build the canonical instrument mapping consumed by bpt-refdata.",
    no_args_is_help=False,
)


@app.callback(invoke_without_command=True)
def run(
    output_dir: Path = typer.Option(
        Path("../config/instruments"),
        "--output-dir",
        "-o",
        help="Where to write instrument_mapping.<exchange>.json files. Default resolves to "
        "<repo-root>/config/instruments/ when invoked from the bpt-ops dir.",
    ),
    exchange: list[str] = typer.Option(
        ["OKX"],  # binance/hyperliquid/deribit still stubbed
        "--exchange",
        "-e",
        help="Exchanges to fetch (case-insensitive). Repeatable.",
    ),
    dry_run: bool = typer.Option(
        False,
        "--dry-run",
        help="Fetch + reconcile but skip writing files; print a summary instead.",
    ),
) -> None:
    selected = [ExchangeId[name.upper()] for name in exchange]
    typer.echo(f"[instrument-mapping] fetching {[e.name for e in selected]}")

    raws: list[RawInstrument] = []
    for ex in selected:
        batch = fetch_for(ex)
        typer.echo(f"[instrument-mapping]   {ex.name}: {len(batch)} instruments")
        raws.extend(batch)

    mapping = reconcile.build(raws)
    typer.echo(
        f"[instrument-mapping] reconciled: {mapping.instrument_count} canonical instruments"
    )

    if dry_run:
        typer.echo(f"[instrument-mapping] --dry-run: skipping write to {output_dir}")
        return

    written = emit.write_per_exchange(mapping, output_dir)
    for path in written:
        typer.echo(f"[instrument-mapping] wrote {path}")
