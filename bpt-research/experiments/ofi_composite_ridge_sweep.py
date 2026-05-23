"""Ridge regression sweep on per-instrument composite weights.

The gate sweep (composite_gate_sweep) ruled out *sparsity* (all-or-nothing
weight zeroing) as a fix for the 35.7% residual loss in the per-instrument
composite. The natural follow-up is *proportional shrinkage* — ridge
regression, which pulls all weights toward zero in proportion to their
magnitude rather than thresholding.

Hypothesis: OLS's per-instrument weights are slightly over-fit on the
training window (especially the smaller weight on OFI-dominant
instruments like AAVE). A small λ shrinks both weights proportionally —
the dominant weight barely changes, but the small weight shrinks toward
zero, which should help in cases where the small weight is noise without
the all-or-nothing downside the gate had.

Sweep λ across orders of magnitude; pick the level that maximises OOS IC.

Output: ridge mean IC at each λ, per-instrument detail at the best λ.
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
LAMBDAS = [0.0, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7]


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


def fit_ridge_per_instrument(
    train_frames: list[pd.DataFrame],
    lambda_: float,
) -> dict[str, float]:
    """Per-instrument ridge: pool train rows, z-score features per cell,
    fit `ret ~ z(ofi) + z(microprice_dev) + const` with L2 penalty on
    slopes (NOT on intercept). λ=0 → OLS."""
    zs_o, zs_m, rets = [], [], []
    for df in train_frames:
        zs_o.append(_z(df.ofi.values))
        zs_m.append(_z(df.microprice_dev.values))
        rets.append(df.ret_1s.values)
    Z_o = np.concatenate(zs_o); Z_m = np.concatenate(zs_m); Y = np.concatenate(rets)
    X = np.column_stack([Z_o, Z_m, np.ones(len(Y))])
    XtX = X.T @ X
    reg = np.eye(X.shape[1]) * lambda_
    reg[-1, -1] = 0.0  # intercept unpenalised
    coef = np.linalg.solve(XtX + reg, X.T @ Y)
    return {'ofi': float(coef[0]), 'microprice_dev': float(coef[1])}


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
    def _ic(col):
        S = np.concatenate([_z(df[col].values) for df in test_frames])
        Y = np.concatenate([df.ret_1s.values for df in test_frames])
        valid = ~np.isnan(S) & ~np.isnan(Y)
        if int(valid.sum()) < 100:
            return float('nan')
        rho, _ = spearmanr(S[valid], Y[valid])
        return float(rho)
    return max(_ic('ofi'), _ic('microprice_dev'))


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

    best_single_by_iid = {iid: best_single_ic(test_by[iid]) for iid in common}

    # Sweep λ
    print('\n=== Lambda sweep ===')
    sweep_rows = []
    for lam in LAMBDAS:
        ics = []
        wins = 0
        # Track magnitude of weight shrinkage relative to OLS
        sum_shrinkage = 0.0
        for iid in common:
            w = fit_ridge_per_instrument(train_by[iid], lam)
            ic = score(test_by[iid], w)
            ics.append(ic)
            if ic > best_single_by_iid[iid]:
                wins += 1
        arr = np.array(ics)
        sweep_rows.append({
            'lambda': lam,
            'mean_ic': float(np.nanmean(arr)),
            'median_ic': float(np.nanmedian(arr)),
            'pos_frac': float(np.nanmean(arr > 0)),
            'wins_vs_best_single': wins,
            'win_rate': wins / len(common),
        })
    sweep = pd.DataFrame(sweep_rows)
    print(sweep.to_string(index=False, float_format=lambda v: f'{v:.4f}'))

    # Best λ
    best_row = sweep.loc[sweep.mean_ic.idxmax()]
    lam_best = float(best_row['lambda'])
    print(f'\n=== Best λ = {lam_best:.0e}: '
          f'mean_ic={best_row.mean_ic:+.4f}, '
          f'win_rate={best_row.win_rate:.1%} ===')

    # Per-instrument detail at OLS (λ=0) vs best λ
    detail_rows = []
    for iid in common:
        w_ols = fit_ridge_per_instrument(train_by[iid], 0.0)
        w_ridge = fit_ridge_per_instrument(train_by[iid], lam_best)
        ic_ols = score(test_by[iid], w_ols)
        ic_ridge = score(test_by[iid], w_ridge)
        detail_rows.append({
            'symbol': iid2sym.get(iid, str(iid)),
            'instrument_id': iid,
            'w_ofi_ols': w_ols['ofi'],
            'w_mp_ols': w_ols['microprice_dev'],
            'w_ofi_ridge': w_ridge['ofi'],
            'w_mp_ridge': w_ridge['microprice_dev'],
            'ic_ols': ic_ols,
            'ic_ridge': ic_ridge,
            'ridge_uplift': ic_ridge - ic_ols,
            'ic_best_single': best_single_by_iid[iid],
            'uplift_vs_best': ic_ridge - best_single_by_iid[iid],
        })
    df = pd.DataFrame(detail_rows)
    df.to_csv('/tmp/bpt_canon/composite_ridge_sweep.csv', index=False)
    sweep.to_csv('/tmp/bpt_canon/composite_ridge_sweep_summary.csv', index=False)

    # Largest uplifts (where ridge helped)
    print('\n=== Top 10 instruments helped by ridge (vs OLS) ===')
    helped = df.sort_values('ridge_uplift', ascending=False).head(10)
    print(helped[['symbol', 'w_ofi_ols', 'w_mp_ols',
                  'w_ofi_ridge', 'w_mp_ridge',
                  'ic_ols', 'ic_ridge', 'ridge_uplift']].to_string(index=False))

    print('\n=== Top 10 instruments hurt by ridge (vs OLS) ===')
    hurt = df.sort_values('ridge_uplift').head(10)
    print(hurt[['symbol', 'w_ofi_ols', 'w_mp_ols',
                'w_ofi_ridge', 'w_mp_ridge',
                'ic_ols', 'ic_ridge', 'ridge_uplift']].to_string(index=False))

    print('\nwrote /tmp/bpt_canon/composite_ridge_sweep{,_summary}.csv')
    return 0


if __name__ == '__main__':
    sys.exit(main())
