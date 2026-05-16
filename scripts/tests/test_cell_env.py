import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

import pytest  # noqa: E402

from sweep_lib.cell_env import CellEnvironment, allocate_cells  # noqa: E402


def base_cfg() -> dict:
    return {
        "aeron": {"media_driver_dir": "/dev/shm/aeron-bpt"},
        "metrics": {"port": 9100},
        "logging": {"dir": "logs"},
        "endpoints": {
            "binance_md_port": 9100,
            "okx_md_port": 9101,
            "hyperliquid_md_port": 9102,
            "binance_order_port": 9110,
            "hyperliquid_info_port": 9114,
        },
    }


def test_index_zero_preserves_legacy_paths():
    env = CellEnvironment(index=0)
    out = env.apply_to_instance_cfg(base_cfg())
    assert out["aeron"]["media_driver_dir"] == "/dev/shm/aeron-bpt"
    assert out["metrics"]["port"] == 9100
    assert out["logging"]["dir"] == "logs"
    assert out["endpoints"]["binance_md_port"] == 9100  # unchanged


def test_index_nonzero_uses_unique_shm_and_port():
    env = CellEnvironment(index=2)
    out = env.apply_to_instance_cfg(base_cfg())
    assert out["aeron"]["media_driver_dir"] == "/dev/shm/aeron-bpt-2"
    assert out["metrics"]["port"] == 9100 + 2 * 100  # 9300
    assert out["logging"]["dir"] == "logs/cell-2"


def test_index_nonzero_offsets_endpoint_ports():
    env = CellEnvironment(index=1)
    out = env.apply_to_instance_cfg(base_cfg())
    # All ports shift by +stride=100.
    assert out["endpoints"]["binance_md_port"] == 9200
    assert out["endpoints"]["okx_md_port"] == 9201
    assert out["endpoints"]["hyperliquid_md_port"] == 9202
    assert out["endpoints"]["binance_order_port"] == 9210
    assert out["endpoints"]["hyperliquid_info_port"] == 9214


def test_apply_does_not_mutate_input():
    cfg = base_cfg()
    snapshot = {k: dict(v) if isinstance(v, dict) else v for k, v in cfg.items()}
    env = CellEnvironment(index=3)
    env.apply_to_instance_cfg(cfg)
    assert cfg == snapshot


def test_allocate_cells_indices_dense():
    cells = allocate_cells(4)
    assert [c.index for c in cells] == [0, 1, 2, 3]


def test_allocate_cells_rejects_zero_parallel():
    with pytest.raises(ValueError):
        allocate_cells(0)


def test_unique_shm_paths_across_cells():
    cells = allocate_cells(5)
    paths = [c.media_driver_dir for c in cells]
    assert len(set(paths)) == 5  # all unique


def test_metrics_ports_are_unique():
    cells = allocate_cells(8)
    ports = [c.metrics_port for c in cells]
    assert len(set(ports)) == 8


def test_endpoint_offsets_dont_overlap_at_default_stride():
    # Stride 100; default ports span ~15 each. Two adjacent cells must
    # never collide on any single port.
    cell0 = CellEnvironment(index=0)
    cell1 = CellEnvironment(index=1)
    cfg = base_cfg()
    out0 = cell0.apply_to_instance_cfg(cfg)
    out1 = cell1.apply_to_instance_cfg(cfg)
    ports0 = set(out0["endpoints"].values())
    ports1 = set(out1["endpoints"].values())
    assert ports0.isdisjoint(ports1)
