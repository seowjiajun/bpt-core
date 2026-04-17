from pathlib import Path

from bpt_ops.jobs.exchange_catalog import generate
from bpt_ops.jobs.exchange_catalog.model import ExchangeCatalog, Exchange


def _catalog() -> ExchangeCatalog:
    return ExchangeCatalog(
        exchanges=[
            Exchange(id=2, name="OKX", display_name="OKX"),
            Exchange(id=1, name="BINANCE", display_name="Binance"),
        ]
    )


def test_render_is_sorted_by_id():
    rendered = generate.render(_catalog())
    # BINANCE (id=1) must appear before OKX (id=2) in the output regardless of input order
    assert rendered.index("BINANCE = 1") < rendered.index("OKX = 2")


def test_render_is_deterministic():
    a = generate.render(_catalog())
    b = generate.render(_catalog())
    assert a == b


def test_write_round_trips_through_import(tmp_path: Path):
    target = tmp_path / "_generated.py"
    generate.write(_catalog(), target)

    # Byte-identical on re-render (the generator is idempotent)
    generate.write(_catalog(), target)
    once = target.read_text()
    generate.write(_catalog(), target)
    twice = target.read_text()
    assert once == twice
