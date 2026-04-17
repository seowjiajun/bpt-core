import typer

from bpt_ops.jobs.instrument_mapping.cli import app as instrument_mapping_app

app = typer.Typer(
    name="bpt-ops",
    help="Back-office batch jobs for the bpt trading system.",
    no_args_is_help=True,
    add_completion=False,
)
app.add_typer(instrument_mapping_app, name="instrument-mapping")


if __name__ == "__main__":
    app()
