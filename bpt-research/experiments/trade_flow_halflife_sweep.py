"""Halflife sweep on trade_flow_ewma.

The 3-feature composite study used halflife_s=1.0 — an unmotivated guess.
At that setting trade-flow's standalone median IC was only +0.007 across
182 instruments, too weak to materially lift the per-instrument composite
over the 2-feature baseline despite being well-orthogonal (Spearman ~0.05
to both OFI and microprice).

This sweep asks: does a different halflife produce a *stronger* standalone
signal? If yes, the 3-feature composite story changes. If no across 4
orders of magnitude of halflife, trade_flow_ewma is a dead end — pivot to
a different 3rd-feature candidate.

Per halflife:
  1. Build trade_flow_ewma feature at that halflife
  2. Standalone median IC across all instruments on test
  3. Standalone pos_frac (% instruments with positive IC)
  4. Pairwise Spearman with OFI and microprice (orthogonality check —
     might also change with halflife)
  5. 3-feature per-instrument composite OOS mean IC + win rate vs 2-feat

Reuses BBO + trade extraction; only the EWMA computation varies by λ.
"""

from __future__ import annotations

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
from ic import ofi, microprice_dev, trade_flow_ewma  # noqa: E402
from ic.panel import _prepare_bbo, _forward_return  # noqa: E402
from ic.multivariate import _z  # noqa: E402

HORIZON_NS = 1_000_000_000
MIN_TICKS = 500
HALFLIVES = [0.1, 0.5, 1.0, 5.0, 30.0, 300.0]


def gather(canon_paths: list[Path]) -> dict[int, list[dict]]:
    """Per instrument, list of per-day dicts with:
       bbo_df, trades_df_i, ofi, microprice_dev, ret_1s
    trade_flow_ewma is computed downstream per halflife.
    """
    by_iid: dict[int, list[dict]] = {}
    for cp in canon_paths:
        bbo = _prepare_bbo(bc.read_bbos(cp))
        trades = bc.read_trades(cp)
        for iid, grp in bbo.groupby('instrument_id', sort=True):
            grp = grp.reset_index(drop=True)
            ret = _forward_return(grp, HORIZON_NS).values
            f_o = ofi(grp).values
            f_m = microprice_dev(grp).values
            tr_i = trades[trades.instrument_id == iid].reset_index(drop=True)
            mask = ~np.isnan(ret) & ~np.isnan(f_o) & ~np.isnan(f_m)
            if int(mask.sum()) < MIN_TICKS:
                continue
            by_iid.setdefault(int(iid), []).append({
                'bbo': grp,
                'trades': tr_i,
                'ofi': f_o,
                'microprice_dev': f_m,
                'ret_1s': ret,
                'mask_no_tf': mask,
                'day': cp.stem,
            })
    return by_iid


def materialize_with_halflife(
    cells: list[dict],
    halflife_s: float,
) -> list[pd.DataFrame]:
    """For each cell, attach trade_flow_ewma at given halflife and produce
    a DataFrame restricted to fully-valid rows (incl. trade_flow NaN check)."""
    out = []
    for c in cells:
        tf = trade_flow_ewma(c['bbo'], c['trades'], halflife_s=halflife_s).values
        mask = c['mask_no_tf'] & ~np.isnan(tf)
        if int(mask.sum()) < MIN_TICKS:
            continue
        out.append(pd.DataFrame({
            'ofi':            c['ofi'][mask],
            'microprice_dev': c['microprice_dev'][mask],
            'trade_flow':     tf[mask],
            'ret_1s':         c['ret_1s'][mask],
        }))
    return out


def fit_ols(frames: list[pd.DataFrame], cols: list[str]) -> dict[str, float]:
    zs = {c: [] for c in cols}
    rets = []
    for df in frames:
        for c in cols:
            zs[c].append(_z(df[c].values))
        rets.append(df.ret_1s.values)
    Z = np.column_stack([np.concatenate(zs[c]) for c in cols])
    Y = np.concatenate(rets)
    X = np.column_stack([Z, np.ones(len(Y))])
    coef, *_ = np.linalg.lstsq(X, Y, rcond=None)
    return {cols[i]: float(coef[i]) for i in range(len(cols))}


def score_composite(frames: list[pd.DataFrame],
                     w: dict[str, float]) -> float:
    sigs, rets = [], []
    for df in frames:
        s = np.zeros(len(df))
        for c, val in w.items():
            s += val * _z(df[c].values)
        sigs.append(s); rets.append(df.ret_1s.values)
    S = np.concatenate(sigs); Y = np.concatenate(rets)
    valid = ~np.isnan(S) & ~np.isnan(Y)
    if int(valid.sum()) < 100:
        return float('nan')
    rho, _ = spearmanr(S[valid], Y[valid])
    return float(rho)


def score_single(frames: list[pd.DataFrame], col: str) -> float:
    S = np.concatenate([_z(df[col].values) for df in frames])
    Y = np.concatenate([df.ret_1s.values for df in frames])
    valid = ~np.isnan(S) & ~np.isnan(Y)
    if int(valid.sum()) < 100:
        return float('nan')
    rho, _ = spearmanr(S[valid], Y[valid])
    return float(rho)


def pairwise_corr(frames: list[pd.DataFrame], a: str, b: str) -> float:
    pooled = pd.concat(frames, ignore_index=True)
    rho, _ = spearmanr(pooled[a], pooled[b])
    return float(rho)


def main() -> int:
    all_canons = sorted(Path('/tmp/bpt_canon').glob('hl-2026-05-*-h00.canon'))
    train, test = all_canons[:7], all_canons[7:]
    print(f'train: {len(train)}d, test: {len(test)}d', flush=True)

    print('gathering train (one pass) ...', flush=True)
    train_by = gather(train)
    print(f'  train instruments: {len(train_by)}', flush=True)
    print('gathering test ...', flush=True)
    test_by = gather(test)
    common = sorted(set(train_by) & set(test_by))
    print(f'  test instruments:  {len(test_by)}', flush=True)
    print(f'  common:            {len(common)}', flush=True)

    print('\n=== Halflife sweep ===')
    print(f'{"halflife_s":>11s}  {"corr_ofi":>9s}  {"corr_mp":>9s}  '
          f'{"med_ic_tf":>10s}  {"pos_tf":>7s}  '
          f'{"3f_mean":>8s}  {"2f_mean":>8s}  {"3v2_win":>8s}', flush=True)

    sweep_rows = []
    for hl in HALFLIVES:
        # Materialize per-cell frames with this halflife
        tr_cells_by = {iid: materialize_with_halflife(train_by[iid], hl)
                        for iid in common}
        te_cells_by = {iid: materialize_with_halflife(test_by[iid], hl)
                        for iid in common}
        # Drop iids where either side ended empty after the trade_flow NaN mask
        valid_iids = [iid for iid in common
                       if tr_cells_by[iid] and te_cells_by[iid]]

        # Per-instrument 2-feat and 3-feat composites; score on test
        ic3, ic2, ic_tf_individual = [], [], []
        wins_3v2 = 0
        for iid in valid_iids:
            w2 = fit_ols(tr_cells_by[iid], ['ofi', 'microprice_dev'])
            w3 = fit_ols(tr_cells_by[iid], ['ofi', 'microprice_dev', 'trade_flow'])
            s2 = score_composite(te_cells_by[iid], w2)
            s3 = score_composite(te_cells_by[iid], w3)
            ic2.append(s2); ic3.append(s3)
            ic_tf_individual.append(score_single(te_cells_by[iid], 'trade_flow'))
            if s3 > s2:
                wins_3v2 += 1
        ic2_arr = np.array(ic2); ic3_arr = np.array(ic3)
        tf_arr = np.array(ic_tf_individual)

        # Pairwise correlations across pooled train cells (orthogonality may
        # change with halflife — verify)
        all_train_frames = [df for iid in valid_iids for df in tr_cells_by[iid]]
        corr_ofi = pairwise_corr(all_train_frames, 'ofi', 'trade_flow')
        corr_mp  = pairwise_corr(all_train_frames, 'microprice_dev', 'trade_flow')

        sweep_rows.append({
            'halflife_s': hl,
            'corr_ofi': corr_ofi,
            'corr_mp': corr_mp,
            'median_ic_tf': float(np.nanmedian(tf_arr)),
            'pos_frac_tf': float(np.nanmean(tf_arr > 0)),
            'mean_ic_3feat': float(np.nanmean(ic3_arr)),
            'mean_ic_2feat': float(np.nanmean(ic2_arr)),
            'win_rate_3v2': wins_3v2 / len(valid_iids),
            'n_instruments': len(valid_iids),
        })
        print(f'  {hl:>9.3f}  {corr_ofi:>+9.3f}  {corr_mp:>+9.3f}  '
              f'{np.nanmedian(tf_arr):>+10.4f}  {(tf_arr > 0).mean():>7.1%}  '
              f'{np.nanmean(ic3_arr):>+8.4f}  {np.nanmean(ic2_arr):>+8.4f}  '
              f'{wins_3v2/len(valid_iids):>8.1%}',
              flush=True)

    sweep_df = pd.DataFrame(sweep_rows)
    sweep_df.to_csv('/tmp/bpt_canon/trade_flow_halflife_sweep.csv', index=False)

    # Pick best (highest mean_ic_3feat that also beats 2feat)
    best = sweep_df.loc[sweep_df.mean_ic_3feat.idxmax()]
    print(f'\nBest halflife: {best.halflife_s}s  →  3-feat OOS IC = {best.mean_ic_3feat:+.4f}  '
          f'(2-feat baseline: {best.mean_ic_2feat:+.4f}, win rate 3v2: {best.win_rate_3v2:.1%})')
    delta = best.mean_ic_3feat - best.mean_ic_2feat
    if delta > 0.001:
        print(f'  → trade-flow at hl={best.halflife_s}s adds +{delta:.4f} IC')
    elif delta < -0.001:
        print(f'  → trade-flow hurts at every halflife (best is still {delta:+.4f})')
    else:
        print(f'  → no halflife produces a meaningful uplift (best Δ = {delta:+.4f})')

    print('\nwrote /tmp/bpt_canon/trade_flow_halflife_sweep.csv')
    return 0


if __name__ == '__main__':
    sys.exit(main())
