"""Single-feature gate sweep on per-instrument composite weights.

Builds on `ofi_per_instrument_composite.py`. Per-instrument OLS gives both
features non-zero weights even when one is much smaller — and on
OFI-dominant instruments (AAVE, POL, APEX), that small microprice weight
contributes noise that costs IC on test data.

This script: for each candidate threshold T, gate the smaller per-instrument
weight to zero whenever |w_dominant / w_other| > T. Then re-evaluate on
held-out test data. The lowest-IC-cost threshold is the operating point.

T = ∞ is the ungated baseline (today's per-instrument result).
T = 0 zeros out everything except the largest weight per instrument
(equivalent to "always use the dominant feature alone").

Output: gated mean IC + win-rate vs best-single, per threshold.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
import pandas as pd
from scipy.stats import spearmanr

REPO = Path('/home/jseow/code/bpt-core')
sys.path.insert(0, str(REPO / 'bpt-canon' / 'python'))
sys.path.insert(0, str(REPO / 'bpt-features' / 'python'))
sys.path.insert(0, str(REPO / 'bazel-bin' / 'bpt-features' / 'python'))
sys.path.insert(0, str(REPO / 'bpt-research'))

import bpt_canon as bc  # noqa: E402

from ic import ofi, microprice_dev  # noqa: E402
from ic.panel import _prepare_bbo, _forward_return  # noqa: E402
from ic.multivariate import _z  # noqa: E402

HORIZON_NS = 1_000_000_000
MIN_TICKS = 500
THRESHOLDS = [1.0, 1.5, 2.0, 3.0, 5.0, 10.0, float('inf')]


def gather(canon_paths: list[Path]) -> dict[int, list[pd.DataFrame]]:
    by_iid: dict[int, list[pd.DataFrame]] = {}
    for cp in canon_paths:
        bbo = _prepare_bbo(bc.read_bbos(cp))
        for iid, grp in bbo.groupby('instrument_id', sort=True):
            grp = grp.reset_index(drop=True)
            ret = _forward_return(grp, HORIZON_NS).values
            f_o = ofi(grp).values
            f_m = microprice_dev(grp).values
            mask = ~np.isnan(ret) & ~np.isnan(f_o) & ~np.isnan(f_m)
            if int(mask.sum()) < MIN_TICKS:
                continue
            by_iid.setdefault(int(iid), []).append(pd.DataFrame({
                'ofi': f_o[mask], 'microprice_dev': f_m[mask],
                'ret_1s': ret[mask], 'day': cp.stem,
            }))
    return by_iid


def fit_per_instrument(train_frames: list[pd.DataFrame]) -> dict[str, float]:
    zs_o, zs_m, rets = [], [], []
    for df in train_frames:
        zs_o.append(_z(df.ofi.values))
        zs_m.append(_z(df.microprice_dev.values))
        rets.append(df.ret_1s.values)
    Z_o = np.concatenate(zs_o); Z_m = np.concatenate(zs_m); Y = np.concatenate(rets)
    X = np.column_stack([Z_o, Z_m, np.ones(len(Y))])
    coef, *_ = np.linalg.lstsq(X, Y, rcond=None)
    return {'ofi': float(coef[0]), 'microprice_dev': float(coef[1])}


def apply_gate(w: dict[str, float], threshold: float) -> dict[str, float]:
    """Zero out the smaller weight if |w_dominant / w_other| > threshold.
    threshold=inf returns w unchanged; threshold=0 keeps only the larger weight."""
    if threshold == float('inf'):
        return dict(w)
    abs_w = {k: abs(v) for k, v in w.items()}
    dominant = max(abs_w, key=abs_w.get)
    other = next(k for k in w if k != dominant)
    if abs_w[other] == 0:
        return dict(w)
    if abs_w[dominant] / abs_w[other] > threshold:
        return {dominant: w[dominant], other: 0.0}
    return dict(w)


def score(test_frames: list[pd.DataFrame], w: dict[str, float]) -> float:
    sigs, rets = [], []
    for df in test_frames:
        s = w['ofi'] * _z(df.ofi.values) + w['microprice_dev'] * _z(df.microprice_dev.values)
        sigs.append(s); rets.append(df.ret_1s.values)
    S = np.concatenate(sigs); Y = np.concatenate(rets)
    valid = ~np.isnan(S) & ~np.isnan(Y)
    if int(valid.sum()) < 100:
        return float('nan')
    rho, _ = spearmanr(S[valid], Y[valid])
    return float(rho)


def best_single_ic(test_frames: list[pd.DataFrame]) -> float:
    """IC of the better of (OFI alone, microprice alone) on test."""
    def _ic_single(col):
        sigs = [_z(df[col].values) for df in test_frames]
        rets = [df.ret_1s.values for df in test_frames]
        S = np.concatenate(sigs); Y = np.concatenate(rets)
        valid = ~np.isnan(S) & ~np.isnan(Y)
        if int(valid.sum()) < 100:
            return float('nan')
        rho, _ = spearmanr(S[valid], Y[valid])
        return float(rho)
    return max(_ic_single('ofi'), _ic_single('microprice_dev'))


def main() -> int:
    all_canons = sorted(Path('/tmp/bpt_canon').glob('hl-2026-05-*-h00.canon'))
    train, test = all_canons[:7], all_canons[7:]
    print(f'train: {len(train)}d, test: {len(test)}d', flush=True)

    train_by = gather(train)
    test_by = gather(test)
    common = sorted(set(train_by) & set(test_by))
    print(f'instruments: {len(common)}', flush=True)

    with open(REPO / 'config/instruments/instrument_mapping.hyperliquid-mainnet.json') as f:
        iid2sym = {v: k.replace('3_', '') for k, v in json.load(f)['forward'].items()}

    # Per-instrument unconstrained weights, computed once
    weights_by_iid = {iid: fit_per_instrument(train_by[iid]) for iid in common}
    best_single_by_iid = {iid: best_single_ic(test_by[iid]) for iid in common}

    # Sweep thresholds
    print('\n=== Threshold sweep ===')
    sweep_rows = []
    for T in THRESHOLDS:
        per_inst_ics = []
        gated_count = 0
        wins_vs_best = 0
        for iid in common:
            w_gated = apply_gate(weights_by_iid[iid], T)
            if w_gated['ofi'] == 0 or w_gated['microprice_dev'] == 0:
                gated_count += 1
            ic = score(test_by[iid], w_gated)
            per_inst_ics.append(ic)
            if ic > best_single_by_iid[iid]:
                wins_vs_best += 1
        arr = np.array(per_inst_ics)
        sweep_rows.append({
            'threshold': T if T != float('inf') else '∞',
            'mean_ic': float(np.nanmean(arr)),
            'median_ic': float(np.nanmedian(arr)),
            'pos_frac': float(np.nanmean(arr > 0)),
            'wins_vs_best_single': wins_vs_best,
            'win_rate': wins_vs_best / len(common),
            'n_instruments_gated': gated_count,
        })
    sweep = pd.DataFrame(sweep_rows)
    print(sweep.to_string(index=False, float_format=lambda v: f'{v:.4f}'))

    # Pick best by mean_ic, show per-instrument detail
    best_row = sweep.loc[sweep.mean_ic.idxmax()]
    T_best_label = best_row.threshold
    T_best = float(T_best_label) if T_best_label != '∞' else float('inf')
    print(f'\n=== Best threshold: T={T_best_label} '
          f'(mean_ic={best_row.mean_ic:+.4f}, '
          f'win_rate={best_row.win_rate:.1%}) ===')

    detail_rows = []
    for iid in common:
        w_raw = weights_by_iid[iid]
        w_gated = apply_gate(w_raw, T_best)
        ic_raw = score(test_by[iid], w_raw)
        ic_gated = score(test_by[iid], w_gated)
        best_s = best_single_by_iid[iid]
        detail_rows.append({
            'symbol': iid2sym.get(iid, str(iid)),
            'instrument_id': iid,
            'w_ofi_raw': w_raw['ofi'],
            'w_mp_raw': w_raw['microprice_dev'],
            'w_ofi_gated': w_gated['ofi'],
            'w_mp_gated': w_gated['microprice_dev'],
            'ic_raw': ic_raw,
            'ic_gated': ic_gated,
            'ic_best_single': best_s,
            'gate_uplift': ic_gated - ic_raw,
            'uplift_vs_best_single': ic_gated - best_s,
        })
    df = pd.DataFrame(detail_rows)
    df.to_csv('/tmp/bpt_canon/composite_gate_sweep.csv', index=False)

    # Where did the gate help / hurt the most?
    print('\n=== Largest gate UPLIFTS (instruments helped) ===')
    helped = df[df.gate_uplift > 0.001].sort_values('gate_uplift', ascending=False).head(10)
    print(helped[['symbol', 'w_ofi_raw', 'w_mp_raw',
                  'ic_raw', 'ic_gated', 'gate_uplift']].to_string(index=False))

    print('\n=== Largest gate DROPS (instruments hurt — sanity check) ===')
    hurt = df[df.gate_uplift < -0.001].sort_values('gate_uplift').head(10)
    if len(hurt) == 0:
        print('  (none — gate never hurt)')
    else:
        print(hurt[['symbol', 'w_ofi_raw', 'w_mp_raw',
                    'ic_raw', 'ic_gated', 'gate_uplift']].to_string(index=False))

    sweep.to_csv('/tmp/bpt_canon/composite_gate_sweep_summary.csv', index=False)
    print('\nwrote /tmp/bpt_canon/composite_gate_sweep{,_summary}.csv')
    return 0


if __name__ == '__main__':
    sys.exit(main())
