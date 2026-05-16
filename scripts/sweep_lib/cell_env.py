"""Per-cell isolation for parallel sweep runs.

The Aeron media driver is single-tenant on `media_driver_dir` (default
/dev/shm/aeron-bpt). Two backtest stacks running concurrently against the
same media_driver_dir collide on shm. To run cells in parallel we hand
each one a unique:
  - media_driver_dir       → /dev/shm/aeron-bpt-<idx>
  - metrics port           → 9100 + idx (one per stack-component, offset
                             from a configurable port window base)
  - log dir                → bpt-backtester/logs/sweep-<idx>/
  - results-dir suffix     → optional; lets the aggregator dedupe runs

This module is pure config — it produces a CellEnvironment dict that the
runner deep-merges into the strategy's instance config before each cell.
The runner itself spawns the subprocess.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any


@dataclass
class CellEnvironment:
    """Isolation parameters for one parallel cell.

    The defaults are designed so that cell index 0 == legacy single-cell
    behaviour (same shm path, same port window) for back-compat with
    sweeps that don't opt into parallelism.
    """

    index: int = 0
    media_driver_root: str = "/dev/shm/aeron-bpt"
    metrics_port_base: int = 9100  # bpt-backtester default
    metrics_port_stride: int = 100  # 100-port window per cell — enough room for all 6 services

    # Path overrides — populated lazily by `apply_to_instance_cfg`. The
    # base instance config already has [aeron] media_driver_dir, [metrics] port;
    # we overwrite both. log/results dirs are also overridable here.
    log_subdir: str = ""  # filled in apply_to_instance_cfg

    @property
    def media_driver_dir(self) -> str:
        # Index 0 → legacy single-cell path. Higher indices get unique dirs.
        if self.index == 0:
            return self.media_driver_root
        return f"{self.media_driver_root}-{self.index}"

    @property
    def metrics_port(self) -> int:
        return self.metrics_port_base + self.index * self.metrics_port_stride

    def apply_to_instance_cfg(self, cfg: dict) -> dict:
        """Returns a deep-merged copy of `cfg` with isolation paths set.

        Modifies (or creates):
          - [aeron] media_driver_dir
          - [metrics] port
          - [logging] dir   ← prefix with cell index when > 0
          - [endpoints] every *_port — offset by index * stride so order
            servers / md servers / info server don't collide on bind
        """
        import copy

        out = copy.deepcopy(cfg)

        # Aeron media driver directory.
        out.setdefault("aeron", {})["media_driver_dir"] = self.media_driver_dir

        # Prometheus / metrics port.
        out.setdefault("metrics", {})["port"] = self.metrics_port

        # Log dir suffix.
        if self.index > 0:
            out.setdefault("logging", {})
            base = out["logging"].get("dir", "logs")
            out["logging"]["dir"] = f"{base}/cell-{self.index}"

        # WS server ports — offset all of them by the same stride so the
        # gateway → server connection still finds them. Each cell gets a
        # contiguous block, so cell 0 keeps the legacy ports.
        if self.index > 0:
            ep = out.setdefault("endpoints", {})
            offset = self.index * self.metrics_port_stride
            for key in (
                "binance_md_port",
                "okx_md_port",
                "hyperliquid_md_port",
                "deribit_md_port",
                "binance_order_port",
                "okx_order_port",
                "hyperliquid_order_port",
                "deribit_order_port",
                "hyperliquid_info_port",
            ):
                # Only offset ports that the base config explicitly sets; leave
                # missing keys alone so the C++ defaults still apply for
                # the cell-0 stack.
                if key in ep:
                    ep[key] = ep[key] + offset

        return out


def allocate_cells(parallel: int) -> list[CellEnvironment]:
    """Returns `parallel` cell envs, indexed 0..parallel-1."""
    if parallel < 1:
        raise ValueError("parallel must be ≥ 1")
    return [CellEnvironment(index=i) for i in range(parallel)]
