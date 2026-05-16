"""Subprocess runner for the multi-process backtest stack.

A thin wrapper around backtest.sh that gives the gate code a typed
interface: run a backtest with given configs, get back a path to the
produced result dir. Failures throw — the gate caller decides what to
do with them.
"""

from __future__ import annotations

import hashlib
import os
import pathlib
import subprocess
import time

REPO = pathlib.Path(__file__).resolve().parent.parent.parent  # bpt-core
BACKTEST_SH = REPO / "scripts" / "backtest.sh"
RESULTS_ROOT = REPO / "bpt-backtester" / "results"
LOG_FILE = REPO / "bpt-backtester" / "logs" / "bpt-backtester.log"
DEFAULT_REFDATA_CONFIG = REPO / "bpt-refdata" / "config" / "bpt-refdata.backtest-hl.toml"


class BacktestRunError(RuntimeError):
    pass


def run_backtest(
    strategy_config: pathlib.Path,
    backtester_config: pathlib.Path | None = None,
    refdata_config: pathlib.Path | None = None,
    timeout_s: int = 600,
) -> pathlib.Path:
    """Starts the backtest stack via backtest.sh, polls until completion,
    stops the stack, and returns the result directory path. Raises
    BacktestRunError on timeout or non-zero exit.

    Result dir naming follows backtest.sh's convention: it embeds the
    sha256 of the *strategy_config* file (params_hash). We hash the same
    bytes to find the produced dir.
    """
    env = os.environ.copy()
    if refdata_config:
        env["BPT_REFDATA_CONFIG"] = str(refdata_config)
    elif DEFAULT_REFDATA_CONFIG.exists():
        env["BPT_REFDATA_CONFIG"] = str(DEFAULT_REFDATA_CONFIG)
    if backtester_config:
        env["BACKTESTER_CONFIG"] = str(backtester_config)

    proc = subprocess.run(
        ["bash", str(BACKTEST_SH), "start", str(strategy_config)],
        env=env,
        cwd=REPO,
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        raise BacktestRunError(
            f"backtest.sh start failed (rc={proc.returncode}):\n"
            f"stdout: {proc.stdout[-500:]}\nstderr: {proc.stderr[-500:]}"
        )

    # Poll for completion. backtest.sh start returns once the stack is up;
    # the actual run finishes asynchronously when the backtester writes
    # "Backtest complete" to its log.
    deadline = time.time() + timeout_s
    completed = False
    while time.time() < deadline:
        try:
            data = LOG_FILE.read_text(errors="ignore")
        except FileNotFoundError:
            data = ""
        if "Backtest complete" in data:
            completed = True
            break
        time.sleep(2)

    subprocess.run(["bash", str(BACKTEST_SH), "stop"], cwd=REPO, capture_output=True)

    if not completed:
        raise BacktestRunError(f"backtest did not complete within {timeout_s}s")

    # Find the result dir by params_hash (sha256 of strategy config).
    full_hash = hashlib.sha256(strategy_config.read_bytes()).hexdigest()
    short = full_hash[:8]
    candidates = list(RESULTS_ROOT.glob(f"*_{short}_*"))
    if not candidates:
        raise BacktestRunError(
            f"backtest completed but no result dir found for params_hash={short}"
        )
    # Most recently modified wins (re-runs land in the same name).
    return max(candidates, key=lambda p: p.stat().st_mtime)
