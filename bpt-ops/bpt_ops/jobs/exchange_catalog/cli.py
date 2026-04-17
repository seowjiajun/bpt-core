"""`bpt-ops exchange-catalog` — generate / emit / verify the exchange catalog."""
from __future__ import annotations

import sys
from pathlib import Path

import typer

from bpt_ops.jobs.exchange_catalog import check_cpp, emit, generate, model

app = typer.Typer(
    help="Exchange catalog codegen and verification.",
    no_args_is_help=True,
)

# Repo-relative default paths — these resolve when invoked from bpt-ops/ working dir,
# which is how the CLI is typically run (uv run bpt-ops ...).
DEFAULT_YAML = Path("../messages/exchanges.yaml")
DEFAULT_PY_OUT = Path("bpt_ops/common/_exchanges_generated.py")
DEFAULT_JSON_OUT = Path("../config/exchanges/catalog.json")
DEFAULT_CPP_HEADER = Path("../messages/generated/cpp/messages/ExchangeId.h")


@app.command("generate-python")
def generate_python(
    yaml_path: Path = typer.Option(DEFAULT_YAML, "--yaml"),
    out: Path = typer.Option(DEFAULT_PY_OUT, "--out"),
) -> None:
    """Regenerate bpt_ops/common/_exchanges_generated.py from the YAML source."""
    catalog = model.load(yaml_path)
    generate.write(catalog, out)
    typer.echo(f"[exchange-catalog] wrote {out} ({len(catalog.exchanges)} exchanges)")


@app.command("emit-json")
def emit_json(
    yaml_path: Path = typer.Option(DEFAULT_YAML, "--yaml"),
    out: Path = typer.Option(DEFAULT_JSON_OUT, "--out"),
) -> None:
    """Emit config/exchanges/catalog.json for runtime consumers (dashboard etc.)."""
    catalog = model.load(yaml_path)
    emit.write_catalog_json(catalog, out)
    typer.echo(f"[exchange-catalog] wrote {out} ({len(catalog.exchanges)} exchanges)")


@app.command("check-cpp")
def check_cpp_cmd(
    yaml_path: Path = typer.Option(DEFAULT_YAML, "--yaml"),
    header: Path = typer.Option(DEFAULT_CPP_HEADER, "--header"),
) -> None:
    """Verify the C++ ExchangeId.h enum values match the YAML. Exits non-zero on drift."""
    catalog = model.load(yaml_path)
    findings = check_cpp.check(catalog, header)
    if findings:
        typer.secho("❌ C++ header disagrees with YAML:", fg="red", err=True)
        for f in findings:
            typer.echo(f"   - {f}", err=True)
        raise typer.Exit(1)
    typer.echo(f"✓ {header.name} agrees with {yaml_path.name}.")


@app.command("check-generated")
def check_generated(
    yaml_path: Path = typer.Option(DEFAULT_YAML, "--yaml"),
    current: Path = typer.Option(DEFAULT_PY_OUT, "--current"),
) -> None:
    """Verify the committed _exchanges_generated.py matches what the YAML would produce.
    Fails if someone edited the YAML without running `generate-python`."""
    catalog = model.load(yaml_path)
    expected = generate.render(catalog)
    actual = current.read_text() if current.exists() else ""
    if expected != actual:
        typer.secho(
            f"❌ {current} is stale. Run `bpt-ops exchange-catalog generate-python` and commit.",
            fg="red",
            err=True,
        )
        raise typer.Exit(1)
    typer.echo(f"✓ {current.name} is up to date with {yaml_path.name}.")
