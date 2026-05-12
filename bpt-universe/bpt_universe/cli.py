from __future__ import annotations

from pathlib import Path

import click
import polars as pl

from . import features as feat
from . import io, scoring
from . import report as rpt
from .diff import render_diff


# A single ScoringConfig instance is shared between subcommands. Filters
# default to "the venues + types the AS book actually trades today" — change
# via flags when scoring a new venue or asset class.
def _build_cfg(
    venue: tuple[str, ...],
    inst_type: tuple[str, ...],
    min_score: float,
    min_samples: int,
    min_fills: int,
    min_capture_bps: float | None,
) -> scoring.ScoringConfig:
    return scoring.ScoringConfig(
        venues=venue or scoring.ScoringConfig().venues,
        inst_types=inst_type or scoring.ScoringConfig().inst_types,
        min_score=min_score,
        min_samples=min_samples,
        min_fills=min_fills,
        min_realized_capture_bps=min_capture_bps,
    )


def _run_score(
    refdata_path: Path,
    md_path: Path | None,
    fills_path: Path | None,
    cfg: scoring.ScoringConfig,
) -> pl.DataFrame:
    refdata = io.load_refdata(refdata_path)
    md_features = feat.md_features(io.load_md_samples(md_path)) if md_path else None
    fill_features = feat.fill_features(io.load_fills(fills_path)) if fills_path else None
    return scoring.score(
        feat.metadata_features(refdata),
        md=md_features,
        fills=fill_features,
        cfg=cfg,
    )


@click.group()
def cli() -> None:
    """bpt-universe — offline tradable-universe selector."""


_common_filter_opts = [
    click.option(
        "--venue", multiple=True, help="Repeat to allow multiple. Defaults to OKX + HYPERLIQUID."
    ),
    click.option("--inst-type", multiple=True, help="Repeat. Defaults to PERP + SPOT."),
    click.option("--min-score", type=float, default=0.0, show_default=True),
    click.option(
        "--min-samples",
        type=int,
        default=100,
        show_default=True,
        help="MD samples needed to trust spread features",
    ),
    click.option(
        "--min-fills",
        type=int,
        default=20,
        show_default=True,
        help="Fills needed to trust markout features",
    ),
    click.option(
        "--min-capture-bps",
        type=float,
        default=None,
        help="Hard floor on realized markout-after-fees",
    ),
]


def _add_filter_opts(fn):
    for opt in reversed(_common_filter_opts):
        fn = opt(fn)
    return fn


@cli.command("score")
@click.option(
    "--refdata",
    type=click.Path(exists=True, dir_okay=False, path_type=Path),
    default=Path("/opt/bpt/data/instrument_mapping.json"),
    show_default=True,
)
@click.option(
    "--md-samples", type=click.Path(exists=True, dir_okay=False, path_type=Path), default=None
)
@click.option("--fills", type=click.Path(exists=True, dir_okay=False, path_type=Path), default=None)
@click.option(
    "--out",
    type=click.Path(dir_okay=False, path_type=Path),
    default=Path("candidates.parquet"),
    show_default=True,
)
@_add_filter_opts
def score_cmd(
    refdata,
    md_samples,
    fills,
    out,
    venue,
    inst_type,
    min_score,
    min_samples,
    min_fills,
    min_capture_bps,
):
    """Score the universe and write candidates.parquet."""
    cfg = _build_cfg(venue, inst_type, min_score, min_samples, min_fills, min_capture_bps)
    candidates = _run_score(refdata, md_samples, fills, cfg)
    candidates.write_parquet(out)
    click.echo(f"wrote {out} ({candidates.height} candidates)")


@cli.command("diff")
@click.option(
    "--candidates",
    type=click.Path(exists=True, dir_okay=False, path_type=Path),
    default=Path("candidates.parquet"),
    show_default=True,
)
@click.option(
    "--config",
    type=click.Path(exists=True, dir_okay=False, path_type=Path),
    default=Path("../bpt-strategy/config/strategies/avellaneda_stoikov.toml"),
    show_default=True,
)
@click.option("--top-n", type=int, default=None, help="Trim proposed list to top-N by score")
def diff_cmd(candidates, config, top_n):
    """Print a diff of the proposed instruments list against an existing config."""
    df = pl.read_parquet(candidates)
    text, _, _ = render_diff(config, df, top_n=top_n)
    click.echo(text, nl=False)


@cli.command("report")
@click.option(
    "--candidates",
    type=click.Path(exists=True, dir_okay=False, path_type=Path),
    default=Path("candidates.parquet"),
    show_default=True,
)
@click.option(
    "--config",
    type=click.Path(exists=True, dir_okay=False, path_type=Path),
    default=Path("../bpt-strategy/config/strategies/avellaneda_stoikov.toml"),
    show_default=True,
)
@click.option(
    "--out",
    type=click.Path(dir_okay=False, path_type=Path),
    default=Path("universe-report.md"),
    show_default=True,
)
@click.option(
    "--top-n",
    type=int,
    default=30,
    show_default=True,
    help="Rows shown in the report's top-N table",
)
@click.option(
    "--diff-top-n",
    type=int,
    default=None,
    help="Trim proposed list inside the diff (omit = full ranked list)",
)
def report_cmd(candidates, config, out, top_n, diff_top_n):
    """Render the markdown review report."""
    df = pl.read_parquet(candidates)
    diff_text, to_add, to_remove = render_diff(config, df, top_n=diff_top_n)
    md = rpt.render(df, to_add, to_remove, diff_text, top_n=top_n)
    Path(out).write_text(md)
    click.echo(f"wrote {out} ({len(to_add)} adds, {len(to_remove)} removes)")


if __name__ == "__main__":
    cli()
