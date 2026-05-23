"""Per-instrument composite weight study.

Today's earlier walk-forward showed equal-weight global composite (OFI +
microprice_dev) doesn't beat the best single feature on average — composite
helps where both features are moderate, loses where one dominates. The
natural fix: fit weights *per instrument*. Route OFI-dominant instruments
to OFI, microprice-dominant to microprice, moderate-IC ones to a true
composite.

This script:
1. Splits the 10 canon days into 7-train / 3-test.
2. For each (instrument), pools all train (day × tick) rows; fits
   `ret_1s ~ z(ofi) + z(microprice_dev)` per-instrument via OLS.
3. Builds a per-instrument composite using those weights (still z-scored
   per-cell at evaluation time so the signal is scale-free).
4. Evaluates four signals on TEST cells, per instrument:
     - OFI alone
     - microprice_dev alone
     - global-weight composite (today's earlier baseline)
     - per-instrument composite
5. Reports: who beats whom, by how much, and the failure modes.

Output: /tmp/bpt_canon/per_inst_composite.csv + summary table on stdout.
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

from ic import (  # noqa: E402
    ofi, microprice_dev,
    fit_composite_weights,
)
from ic.panel import _prepare_bbo, _forward_return  # noqa: E402
from ic.multivariate import _z  # noqa: E402

HORIZON_NS = 1_000_000_000
MIN_TICKS = 500


def gather(canon_paths: list[Path]) -> dict[int, list[pd.DataFrame]]:
    """For each instrument, collect a list of per-day frames with cols
    ofi, microprice_dev, ret_1s. Per-cell z-scoring done inside the
    fit / eval steps, not here, so each day's data is preserved as a unit."""
    by_iid: dict[int, list[pd.DataFrame]] = {}
    for cp in canon_paths:
        bbo = _prepare_bbo(bc.read_bbos(cp))
        for iid, grp in bbo.groupby('instrument_id', sort=True):
            grp = grp.reset_index(drop=True)
            ret = _forward_return(grp, HORIZON_NS).values
            f_ofi = ofi(grp).values
            f_mp = microprice_dev(grp).values
            mask = (~np.isnan(ret) & ~np.isnan(f_ofi) & ~np.isnan(f_mp))
            if int(mask.sum()) < MIN_TICKS:
                continue
            df = pd.DataFrame({
                'ofi': f_ofi[mask],
                'microprice_dev': f_mp[mask],
                'ret_1s': ret[mask],
                'day': cp.stem,
            })
            by_iid.setdefault(int(iid), []).append(df)
    return by_iid


def fit_per_instrument(train_frames: list[pd.DataFrame]) -> dict[str, float]:
    """Per-cell z-score then pool and fit. Returns {'ofi': w_o, 'microprice_dev': w_m}.
    On degenerate cells (constant feature) returns zero weights for that feature."""
    zs_ofi, zs_mp, rets = [], [], []
    for df in train_frames:
        zs_ofi.append(_z(df.ofi.values))
        zs_mp.append(_z(df.microprice_dev.values))
        rets.append(df.ret_1s.values)
    Z_ofi = np.concatenate(zs_ofi)
    Z_mp = np.concatenate(zs_mp)
    Y = np.concatenate(rets)
    X = np.column_stack([Z_ofi, Z_mp, np.ones(len(Y))])
    coef, *_ = np.linalg.lstsq(X, Y, rcond=None)
    return {'ofi': float(coef[0]), 'microprice_dev': float(coef[1])}


def score_signal(test_frames: list[pd.DataFrame],
                 signal_fn) -> tuple[float, int]:
    """Pool test cells, build the signal per-cell (so per-cell z-scoring
    semantics are preserved), compute one IC across the pooled cells.
    Returns (ic, n_total)."""
    sigs, rets = [], []
    for df in test_frames:
        s = signal_fn(df)
        if s is None:
            continue
        sigs.append(s)
        rets.append(df.ret_1s.values)
    if not sigs:
        return float('nan'), 0
    S = np.concatenate(sigs)
    Y = np.concatenate(rets)
    valid = ~np.isnan(S) & ~np.isnan(Y)
    if int(valid.sum()) < 100:
        return float('nan'), int(valid.sum())
    rho, _ = spearmanr(S[valid], Y[valid])
    return float(rho), int(valid.sum())


def main() -> int:
    all_canons = sorted(Path('/tmp/bpt_canon').glob('hl-2026-05-*-h00.canon'))
    train = all_canons[:7]
    test = all_canons[7:]
    print(f'train: {len(train)} days, test: {len(test)} days', flush=True)

    print('gathering features per (instrument, day) on train...', flush=True)
    train_by_iid = gather(train)
    print(f'  train: {len(train_by_iid)} instruments', flush=True)

    print('gathering features per (instrument, day) on test...', flush=True)
    test_by_iid = gather(test)
    print(f'  test: {len(test_by_iid)} instruments', flush=True)

    print('fitting global-weight composite on train (one model for all)...', flush=True)
    from ic import fit_composite_weights as _global_fit
    global_w = _global_fit(train, {'ofi': ofi, 'microprice_dev': microprice_dev})
    print(f'  global weights: ofi={global_w["ofi"]:+.6f}  '
          f'mp={global_w["microprice_dev"]:+.6f}', flush=True)

    common = sorted(set(train_by_iid) & set(test_by_iid))
    print(f'instruments with both train + test data: {len(common)}', flush=True)

    with open(REPO / 'config/instruments/instrument_mapping.hyperliquid-mainnet.json') as f:
        m = json.load(f)
    iid2sym = {v: k.replace('3_', '') for k, v in m['forward'].items()}

    rows = []
    for iid in common:
        sym = iid2sym.get(iid, f'iid={iid}')
        train_frames = train_by_iid[iid]
        test_frames = test_by_iid[iid]

        # Per-instrument weights
        per_inst_w = fit_per_instrument(train_frames)

        # Signal: OFI alone (z-scored per-cell)
        ic_ofi, _ = score_signal(test_frames, lambda d: _z(d.ofi.values))

        # Signal: microprice_dev alone
        ic_mp, _ = score_signal(test_frames, lambda d: _z(d.microprice_dev.values))

        # Signal: global-weight composite (z * weight per feature, summed)
        ic_global, _ = score_signal(test_frames, lambda d:
            global_w['ofi'] * _z(d.ofi.values) +
            global_w['microprice_dev'] * _z(d.microprice_dev.values))

        # Signal: per-instrument composite
        ic_per, n_test = score_signal(test_frames, lambda d:
            per_inst_w['ofi'] * _z(d.ofi.values) +
            per_inst_w['microprice_dev'] * _z(d.microprice_dev.values))

        best_single = max(ic_ofi, ic_mp)
        rows.append({
            'symbol': sym,
            'instrument_id': iid,
            'w_ofi': per_inst_w['ofi'],
            'w_mp': per_inst_w['microprice_dev'],
            'ic_ofi': ic_ofi,
            'ic_mp': ic_mp,
            'ic_global': ic_global,
            'ic_per_inst': ic_per,
            'best_single': best_single,
            'uplift_vs_best_single': ic_per - best_single,
            'uplift_vs_global': ic_per - ic_global,
            'n_test': n_test,
        })

    df = pd.DataFrame(rows)
    df.to_csv('/tmp/bpt_canon/per_inst_composite.csv', index=False)

    print('\n=== Headline (out-of-sample, all instruments) ===')
    for col in ['ic_ofi', 'ic_mp', 'ic_global', 'ic_per_inst']:
        v = df[col].dropna()
        print(f'  {col:14s}  mean={v.mean():+.4f}  median={v.median():+.4f}  '
              f'pos_frac={(v > 0).mean():.1%}')

    # Win rates
    wins_per_vs_best = (df.uplift_vs_best_single > 0).sum()
    wins_per_vs_global = (df.uplift_vs_global > 0).sum()
    print(f'\n  per-instrument beats best-single: {wins_per_vs_best}/{len(df)} '
          f'({100*wins_per_vs_best/len(df):.1f}%)  '
          f'mean uplift = {df.uplift_vs_best_single.mean():+.4f}')
    print(f'  per-instrument beats global-comp: {wins_per_vs_global}/{len(df)} '
          f'({100*wins_per_vs_global/len(df):.1f}%)  '
          f'mean uplift = {df.uplift_vs_global.mean():+.4f}')

    # Show top + bottom by per-instrument IC
    print('\n=== Top 15 instruments by per-instrument composite IC ===')
    print(df.sort_values('ic_per_inst', ascending=False).head(15)[
        ['symbol', 'w_ofi', 'w_mp', 'ic_ofi', 'ic_mp', 'ic_per_inst',
         'uplift_vs_best_single']
    ].to_string(index=False))

    print('\nwrote /tmp/bpt_canon/per_inst_composite.csv')
    return 0


if __name__ == '__main__':
    sys.exit(main())
