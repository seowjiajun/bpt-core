from pathlib import Path

from bpt_ops.jobs.exchange_catalog.check_cpp import check
from bpt_ops.jobs.exchange_catalog.model import Exchange, ExchangeCatalog


def _header(tmp_path: Path, body: str) -> Path:
    p = tmp_path / "ExchangeId.h"
    p.write_text(body)
    return p


_GOOD = """
    enum Value {
        ALL = static_cast<std::uint8_t>(0),
        BINANCE = static_cast<std::uint8_t>(1),
        OKX = static_cast<std::uint8_t>(2),
        NULL_VALUE = static_cast<std::uint8_t>(255)
    };
"""

_CATALOG = ExchangeCatalog(
    exchanges=[
        Exchange(id=1, name="BINANCE", display_name="Binance"),
        Exchange(id=2, name="OKX", display_name="OKX"),
    ]
)


def test_matching_header_has_no_findings(tmp_path: Path):
    p = _header(tmp_path, _GOOD)
    assert check(_CATALOG, p) == []


def test_missing_exchange_in_header_flagged(tmp_path: Path):
    body = _GOOD.replace("        OKX = static_cast<std::uint8_t>(2),\n", "")
    p = _header(tmp_path, body)
    findings = check(_CATALOG, p)
    assert any("OKX" in f and "missing from" in f for f in findings)


def test_extra_exchange_in_header_flagged(tmp_path: Path):
    body = _GOOD.replace(
        "        OKX = static_cast<std::uint8_t>(2),\n",
        "        OKX = static_cast<std::uint8_t>(2),\n        COINBASE = static_cast<std::uint8_t>(5),\n",
    )
    p = _header(tmp_path, body)
    findings = check(_CATALOG, p)
    assert any("COINBASE" in f and "missing from YAML" in f for f in findings)


def test_id_disagreement_flagged(tmp_path: Path):
    body = _GOOD.replace(
        "OKX = static_cast<std::uint8_t>(2)", "OKX = static_cast<std::uint8_t>(7)"
    )
    p = _header(tmp_path, body)
    findings = check(_CATALOG, p)
    assert any("OKX" in f and "id=7" in f for f in findings)


def test_sentinels_are_ignored(tmp_path: Path):
    # ALL=0 and NULL_VALUE=255 must not appear in findings
    p = _header(tmp_path, _GOOD)
    findings = check(_CATALOG, p)
    assert not any("ALL" in f or "NULL_VALUE" in f for f in findings)


def test_missing_header_flagged(tmp_path: Path):
    findings = check(_CATALOG, tmp_path / "nonexistent.h")
    assert len(findings) == 1
    assert "not found" in findings[0]
