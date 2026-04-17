from pathlib import Path

import pytest

from bpt_ops.jobs.exchange_catalog.model import load


def _write(tmp_path: Path, body: str) -> Path:
    p = tmp_path / "exchanges.yaml"
    p.write_text(body)
    return p


def test_load_valid(tmp_path: Path):
    p = _write(
        tmp_path,
        """
exchanges:
  - {id: 1, name: BINANCE, display_name: Binance}
  - {id: 2, name: OKX, display_name: OKX}
""",
    )
    catalog = load(p)
    assert [e.name for e in catalog.exchanges] == ["BINANCE", "OKX"]


def test_duplicate_id_rejected(tmp_path: Path):
    p = _write(
        tmp_path,
        """
exchanges:
  - {id: 1, name: BINANCE, display_name: Binance}
  - {id: 1, name: COINBASE, display_name: Coinbase}
""",
    )
    with pytest.raises(ValueError, match="duplicate exchange IDs"):
        load(p)


def test_duplicate_name_rejected(tmp_path: Path):
    p = _write(
        tmp_path,
        """
exchanges:
  - {id: 1, name: BINANCE, display_name: Binance}
  - {id: 2, name: BINANCE, display_name: Other}
""",
    )
    with pytest.raises(ValueError, match="duplicate exchange names"):
        load(p)


def test_lowercase_name_rejected(tmp_path: Path):
    p = _write(
        tmp_path,
        """
exchanges:
  - {id: 1, name: binance, display_name: Binance}
""",
    )
    with pytest.raises(ValueError, match="UPPERCASE"):
        load(p)


def test_id_out_of_uint8_range(tmp_path: Path):
    p = _write(
        tmp_path,
        """
exchanges:
  - {id: 256, name: BIG, display_name: Big}
""",
    )
    with pytest.raises(Exception):  # pydantic ValidationError
        load(p)
