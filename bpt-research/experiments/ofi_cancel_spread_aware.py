"""Spread-aware OFI cancel simulator.

Yesterday's prototype (ofi_cancel_prototype.py) showed cancellation
reduces per-fill adverse markout by +0.50 bps at θ=0.5σ. But it didn't
model the spread AS earns per fill — and cancellation reduces fills,
which reduces both adverse markout (good) AND spread revenue (bad).

This script adds maker_spread_bps as a parameter and computes net PnL
per fill:

  PnL_per_BUY  = +half_spread_bps - markout_bps   (markout >0 = adverse)
  PnL_per_SELL = +half_spread_bps + markout_bps   (markout <0 = adverse)

Sweeps across:
  - cancel threshold θ ∈ {0, 0.5, 1, 1.5, 2, 3, ∞}σ
  - maker spread_bps ∈ {0, 2, 4, 6, 10}  (typical HL maker is ~2; some
    instruments support wider quoting)

The headline: for each spread regime, which θ gives the highest mean
NET PnL per fill? Does cancellation help at all, given the spread
revenue cost?

Reuses the trade-level panel from the prototype — only the PnL
computation differs.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
import pandas as pd

REPO = Path('/home/jseow/code/bpt-core')
sys.path.insert(0, str(REPO / 'bpt-canon' / 'python'))
sys.path.insert(0, str(REPO / 'bpt-features' / 'python'))
sys.path.insert(0, str(REPO / 'bazel-bin' / 'bpt-features' / 'python'))
sys.path.insert(0, str(REPO / 'bpt-research'))

import bpt_canon as bc  # noqa: E402
from ic import ofi  # noqa: E402
from ic.panel import _prepare_bbo  # noqa: E402

MARKOUT_HORIZON_NS = 1_000_000_000
MIN_TRADES_PER_INST = 200

THETA_SWEEP = [0.0, 0.5, 1.0, 1.5, 2.0, 3.0, float('inf')]
SPREAD_SWEEP_BPS = [0.0, 2.0, 4.0, 6.0, 10.0]


def collect_for_instrument(bbo_i: pd.DataFrame,
                            trades_i: pd.DataFrame) -> pd.DataFrame:
    if len(trades_i) == 0 or len(bbo_i) < 100:
        return pd.DataFrame()
    ofi_at_bbo = ofi(bbo_i).values
    bbo_ts = bbo_i['ts_ns'].values
    bbo_mid = bbo_i['mid'].values

    trades_i = trades_i[trades_i['side'] < 2].reset_index(drop=True)
    if len(trades_i) == 0:
        return pd.DataFrame()
    trade_ts = trades_i['ts_ns'].values
    bbo_idx = np.searchsorted(bbo_ts, trade_ts, side='right') - 1
    valid = bbo_idx >= 0
    pre_trade_ofi = np.where(valid, ofi_at_bbo[bbo_idx], np.nan)
    anchor_mid = np.where(valid, bbo_mid[bbo_idx], np.nan)

    future_ts = trade_ts + MARKOUT_HORIZON_NS
    fut_idx = np.searchsorted(bbo_ts, future_ts, side='left')
    fut_valid = fut_idx < len(bbo_ts)
    fut_mid = np.where(fut_valid,
                        bbo_mid[np.minimum(fut_idx, len(bbo_ts) - 1)],
                        np.nan)
    markout_bps = np.log(fut_mid / anchor_mid) * 1e4

    return pd.DataFrame({
        'side': trades_i['side'].values,
        'pre_trade_ofi': pre_trade_ofi,
        'markout_bps': markout_bps,
    }).dropna(subset=['pre_trade_ofi', 'markout_bps'])


def net_pnl_per_fill(
    trades: pd.DataFrame, theta: float, ofi_std: float,
    spread_bps: float,
) -> dict:
    """Returns mean net PnL per fill (bps), cancel rate, n_kept."""
    pos_gate = theta * ofi_std
    neg_gate = -theta * ofi_std
    buy_mask  = trades.side == 0
    sell_mask = trades.side == 1

    if theta == float('inf'):
        cancelled = pd.Series(False, index=trades.index)
    else:
        cancelled = (
            (buy_mask  & (trades.pre_trade_ofi > pos_gate)) |
            (sell_mask & (trades.pre_trade_ofi < neg_gate))
        )
    kept = trades[~cancelled]
    if len(kept) == 0:
        return {'mean_net_pnl_bps': 0.0, 'n_kept': 0, 'cancel_rate': 1.0,
                'mean_adverse_markout_bps': 0.0}

    half_spread = spread_bps / 2.0
    pnl_buy = half_spread - kept.loc[kept.side == 0, 'markout_bps']
    pnl_sell = half_spread + kept.loc[kept.side == 1, 'markout_bps']
    all_pnl = pd.concat([pnl_buy, pnl_sell])
    # Adverse component (positive number = how much markout hurts AS)
    adverse_buy = kept.loc[kept.side == 0, 'markout_bps']
    adverse_sell = -kept.loc[kept.side == 1, 'markout_bps']
    adverse_all = pd.concat([adverse_buy, adverse_sell])

    return {
        'mean_net_pnl_bps': float(all_pnl.mean()),
        'n_kept': int(len(kept)),
        'cancel_rate': float(cancelled.mean()),
        'mean_adverse_markout_bps': float(adverse_all.mean()),
    }


def main() -> int:
    all_canons = sorted(Path('/tmp/bpt_canon').glob('hl-2026-05-*-h00.canon'))
    print(f'canons: {len(all_canons)}', flush=True)
    rows = []
    for cp in all_canons:
        bbo = _prepare_bbo(bc.read_bbos(cp))
        trades = bc.read_trades(cp)
        for iid, grp in bbo.groupby('instrument_id', sort=True):
            grp = grp.reset_index(drop=True)
            tr_i = trades[trades.instrument_id == iid].reset_index(drop=True)
            if len(tr_i) < MIN_TRADES_PER_INST:
                continue
            df = collect_for_instrument(grp, tr_i)
            if df.empty:
                continue
            df['instrument_id'] = int(iid)
            rows.append(df)
        print(f'  {cp.stem}: {sum(len(r) for r in rows)} pooled', flush=True)

    panel = pd.concat(rows, ignore_index=True)
    print(f'\ntotal trades: {len(panel):,}  instruments: '
          f'{panel.instrument_id.nunique()}', flush=True)

    with open(REPO / 'config/instruments/instrument_mapping.hyperliquid-mainnet.json') as f:
        iid2sym = {v: k.replace('3_', '') for k, v in json.load(f)['forward'].items()}

    # Per (spread, θ): mean net PnL across instruments
    sweep_rows = []
    for spread in SPREAD_SWEEP_BPS:
        for theta in THETA_SWEEP:
            per_inst = []
            for iid, grp in panel.groupby('instrument_id'):
                ofi_std = grp.pre_trade_ofi.std()
                if ofi_std == 0:
                    continue
                r = net_pnl_per_fill(grp, theta, ofi_std, spread)
                r['instrument_id'] = iid
                r['theta_sigma'] = theta
                r['spread_bps'] = spread
                per_inst.append(r)
            arr = pd.DataFrame(per_inst)
            sweep_rows.append({
                'spread_bps': spread,
                'theta_sigma': theta,
                'mean_net_pnl_bps_per_fill': float(arr.mean_net_pnl_bps.mean()),
                'median_net_pnl_bps_per_fill': float(arr.mean_net_pnl_bps.median()),
                'mean_cancel_rate': float(arr.cancel_rate.mean()),
                'pos_inst_count': int((arr.mean_net_pnl_bps > 0).sum()),
                'n_instruments': len(arr),
            })

    sweep = pd.DataFrame(sweep_rows)
    sweep.to_csv('/tmp/bpt_canon/ofi_cancel_spread_sweep.csv', index=False)

    # Print spread × θ matrix of mean net PnL per fill
    print('\n=== Mean net PnL per fill (bps), spread × threshold ===')
    pivot_mean = sweep.pivot(index='spread_bps', columns='theta_sigma',
                              values='mean_net_pnl_bps_per_fill')
    cols = sorted(pivot_mean.columns)
    print('spread_bps  ' + '  '.join(
        f'{("∞" if c==float("inf") else f"{c:.1f}σ"):>8s}' for c in cols))
    for sp, row in pivot_mean.iterrows():
        print(f'  {sp:6.1f}    ' + '  '.join(
            f'{row[c]:>+8.3f}' for c in cols))

    print('\n=== Fraction of instruments with positive net PnL ===')
    pivot_pos = sweep.pivot(index='spread_bps', columns='theta_sigma',
                             values='pos_inst_count')
    n_inst = sweep['n_instruments'].iloc[0]
    print(f'(out of {n_inst} instruments per cell)')
    print('spread_bps  ' + '  '.join(
        f'{("∞" if c==float("inf") else f"{c:.1f}σ"):>8s}' for c in cols))
    for sp, row in pivot_pos.iterrows():
        print(f'  {sp:6.1f}    ' + '  '.join(
            f'{row[c]/n_inst:>7.1%}' for c in cols))

    # Best θ for each spread level
    print('\n=== Best θ per spread regime ===')
    for sp in SPREAD_SWEEP_BPS:
        sub = sweep[sweep.spread_bps == sp]
        best = sub.loc[sub.mean_net_pnl_bps_per_fill.idxmax()]
        # Compare to no-cancel baseline (θ=inf)
        baseline = sub[sub.theta_sigma == float('inf')].iloc[0]
        delta = best.mean_net_pnl_bps_per_fill - baseline.mean_net_pnl_bps_per_fill
        t_label = '∞' if best.theta_sigma == float('inf') else f'{best.theta_sigma:.1f}σ'
        print(f'  spread={sp:5.1f}bps: best θ={t_label:>4s}  '
              f'net_pnl/fill={best.mean_net_pnl_bps_per_fill:+.3f}  '
              f'vs no-cancel ({baseline.mean_net_pnl_bps_per_fill:+.3f})  '
              f'uplift={delta:+.3f}  '
              f'cancel_rate={best.mean_cancel_rate:.1%}  '
              f'pos_inst={best.pos_inst_count}/{n_inst}')

    print('\nwrote /tmp/bpt_canon/ofi_cancel_spread_sweep.csv')
    return 0


if __name__ == '__main__':
    sys.exit(main())
