# Hurst regime IC — no stable predictive content on HL perps

**Date:** 2026-05-28
**Status:** Complete — negative result. The Hurst exponent does not
separate trending from mean-reverting regimes in a way that survives
out-of-sample across days or instruments.

## Question

Before building a Hurst-gated strategy (e.g. regime-adaptive γ, or a
momentum/reversion switch), does Hurst actually carry predictive content
on this data? The textbook claim: H > 0.5 → persistent/trending,
H < 0.5 → mean-reverting. If true, we should see forward returns
*continue* the past move in high-H regimes and *reverse* it in low-H.

## Setup

| | |
|---|---|
| Series | BBO mid, resampled to 10s bars |
| Hurst | rolling R/S (rescaled-range) over a 200-bar window (~33 min) |
| Returns | past = r[t−30..t], forward = r[t..t+30] (5 min each) |
| Test | bucket bars into low/mid/high Hurst terciles; measure corr(past, forward) per bucket |
| Read | low-H corr should be **negative** (reversion), high-H **positive** (momentum); spread = high_c − low_c should be large positive |
| Data | ETH, SOL × 3 full days (2026-05-05/06/07). BTC absent from capture. |
| No look-ahead | H_t and past use data ≤ t; forward is strictly future |
| Script | `experiments/hurst_regime_ic.py` |

## Result

```
         day  inst    low_H   low_c    mid_c  high_H  high_c   spread
  2026-05-05   ETH    0.551  +0.020  -0.138   0.679  -0.138  -0.158
  2026-05-05   SOL    0.559  -0.081  -0.054   0.693  -0.115  -0.034
  2026-05-06   ETH    0.536  -0.237  +0.071   0.659  -0.003  +0.234
  2026-05-06   SOL    0.564  -0.112  -0.026   0.698  -0.121  -0.009
  2026-05-07   ETH    0.520  -0.101  +0.042   0.662  +0.025  +0.126
  2026-05-07   SOL    0.519  +0.022  +0.045   0.672  -0.057  -0.080
```

Summary across 6 cells: spread mean +0.013 (range −0.158 … +0.234);
spread > 0 in only **2 of 6** cells; low-H reverts (corr<0) in 4/6.

## Findings

**1. The regime-separation spread is noise.** It is positive in 2 of 6
cells and swings from −0.158 to +0.234 — wider than its mean (+0.013).
ETH alone goes −0.158 → +0.234 → +0.126 on three *consecutive* days.
There is no stable sign.

**2. The "high-H = momentum" leg is outright false.** High-H corr is
**negative in 5 of 6 cells** (mean −0.068). High Hurst does not predict
trend persistence here; if anything high-H regimes also mildly revert.

**3. The "low-H = reversion" leg is weak and unstable.** corr<0 in 4/6
(mean −0.081). There is a faint tendency for low-Hurst bars to revert,
but it is not cleanly gated by Hurst — e.g. ETH/05-05 reverts hardest in
the *high*-H bucket, the opposite of the hypothesis.

**4. ETH/05-07 (spread +0.126) was in-sample luck.** It read as a clean
"low reverts, high trends" result. ETH/05-05 has the *opposite* sign
(−0.158). Picking the day that confirms the hypothesis is the exact trap
the spread-floor study fell into.

**5. Hurst describes the past, it does not forecast the regime.** It is
computed *on the same return series* it is meant to predict, so it
mechanically tracks realized recent behavior. That makes it a lagging
descriptor, not a forward signal — consistent with the literature.

## Implications

- **No Hurst-gated strategy.** Do not build regime-adaptive γ or a
  momentum/reversion switch keyed on Hurst — the gate carries no stable
  edge. The existing `compute_hurst` / `RegimeDetector` in bpt-features
  stay as descriptive instrumentation only.
- **The one semi-consistent effect is weak short-horizon reversion**
  (the low-H leg, and parts of mid/high), *not* specific to Hurst. But
  short-horizon mean-reversion is just the market-making bet restated —
  provide liquidity, profit when price comes back — and baseline AS,
  which already makes that bet, tests net-negative after costs. So this
  is not new alpha; it is the same negative result from a different angle.

## Caveats

1. One parameterization (10s bar, 200-bar Hurst window, 5-min horizon).
   A different timescale might read differently — but sweeping bar/window/
   horizon until Hurst looks good would be p-hacking, the same trap as the
   2-feature/halflife sweeps. Tested the natural config; it failed.
2. Three days, two instruments (BTC was not in the capture's subscription
   set — zero BBOs). More instruments/days would tighten the estimate but
   are very unlikely to manufacture a stable sign from a ±0.2 swing.
3. R/S Hurst, not the production `compute_hurst` (DFA-ish, in `_core.so`,
   env-blocked here). R/S is a standard estimator; the conclusion is about
   whether *any* Hurst estimate carries regime content, not the estimator.

## Reproduce

```bash
# 1. generate canon from tape (per day, ~1.4 GB each)
for d in 2026-05-05 2026-05-06 2026-05-07; do
  bazel-bin/bpt-canon/bpt-canon-replay \
    --wslog /opt/bpt/data/raw/hyperliquid/$d/*.wslog \
    --instrument-mapping config/instruments/instrument_mapping.hyperliquid-mainnet.json \
    --output /tmp/hurst_canon/hl-$d.canon
done
# 2. run the IC sweep (pure-python, no _core.so)
python3 bpt-research/experiments/hurst_regime_ic.py
```
