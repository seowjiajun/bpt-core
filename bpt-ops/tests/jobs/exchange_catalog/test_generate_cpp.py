from pathlib import Path

from bpt_ops.jobs.exchange_catalog import generate_cpp
from bpt_ops.jobs.exchange_catalog.model import ExchangeCatalog, Exchange


def _catalog() -> ExchangeCatalog:
    return ExchangeCatalog(
        exchanges=[
            Exchange(id=2, name="OKX", display_name="OKX"),
            Exchange(id=1, name="BINANCE", display_name="Binance"),
        ]
    )


def test_render_is_sorted_by_id():
    rendered = generate_cpp.render(_catalog())
    # BINANCE (id=1) must appear before OKX (id=2) in the table regardless of input order
    assert rendered.index("ExchangeId::BINANCE") < rendered.index("ExchangeId::OKX")


def test_render_is_deterministic():
    a = generate_cpp.render(_catalog())
    b = generate_cpp.render(_catalog())
    assert a == b


def test_render_includes_all_required_pieces():
    rendered = generate_cpp.render(_catalog())
    # Auto-gen banner
    assert "AUTO-GENERATED" in rendered
    assert "generate-cpp-registry" in rendered
    # Pulls in the SBE enum
    assert '#include "ExchangeId.h"' in rendered
    # Lookup helpers
    assert "from_name" in rendered
    assert "display_name" in rendered
    # Constexpr table sized to the catalog
    assert "kEntries" in rendered
    assert "std::array<Entry, 2>" in rendered
    # Each entry rendered with name + display_name
    assert '"BINANCE", "Binance"' in rendered
    assert '"OKX", "OKX"' in rendered


def test_write_round_trips_byte_identical(tmp_path: Path):
    target = tmp_path / "ExchangeRegistry.h"
    generate_cpp.write(_catalog(), target)
    once = target.read_text()
    generate_cpp.write(_catalog(), target)
    twice = target.read_text()
    assert once == twice
