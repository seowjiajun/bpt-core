# Multi-instrument OFI cancel — no deployable edge across APE/XMR/HYPE

**Date:** 2026-05-28
**Status:** Complete — negative result. OFI cancel does not produce a
statistically significant PnL edge on any of the three viable instruments.

## Question

The OFI cancel rule showed a +0.915 bps/fill uplift in the per-fill
markout sim (2026-05-24) but failed to translate to deployable PnL on
APE alone (2026-05-26, full-N sweep: DSR < 0.95 at all θ). Now that the
backtester handles instruments beyond APE (szDecimals tick/lot fix +
order_book_depth fix, 2026-05-28), does OFI cancel help on a broader
instrument set?

## Setup

| | |
|---|---|
| Instruments | APE, XMR, HYPE (the 3 that fill at AS's ~2 bps spread) |
| Sizing | ~$80 notional each (APE 500, XMR 0.209, HYPE 1.41), max_inv 10x |
| θ sweep | off, 0.5, 1.0, 2.0σ (θ=0 dropped — too aggressive, prior finding) |
| Data | 2026-05-07, all 24 hours |
| Cells | 3 coins × 4 θ × 24 hours = 288 backtests |
| Significance | PSR + DSR (Bailey-López de Prado), per (coin, θ) pooled across hours |
| Script | /tmp/sweep_multi_ofi.sh + /tmp/agg_multi_ofi.py |

## Result

| coin | θ | fills | pnl_$ | SR | PSR | DSR |
|---|---|---:|---:|---:|---:|---:|
| APE | off | 51 | −0.244 | −0.023 | 0.434 | 0.201 |
| APE | 0.5 | 29 | −0.607 | −0.095 | 0.293 | 0.139 |
| APE | 1.0 | 37 | −2.275 | −0.236 | 0.026 | 0.003 |
| APE | 2.0 | 47 | −0.850 | −0.099 | 0.228 | 0.074 |
| XMR | off | 9 | +0.164 | +0.088 | 0.601 | 0.139 |
| XMR | 0.5 | 3 | −0.305 | −0.577 | 0.173 | 0.045 |
| XMR | 1.0 | 4 | −0.133 | −0.861 | 0.058 | 0.008 |
| XMR | 2.0 | 11 | −0.222 | −0.081 | 0.397 | 0.041 |
| HYPE | off | 13 | −0.120 | −0.055 | 0.425 | 0.384 |
| HYPE | 0.5 | 8 | −0.008 | −0.004 | 0.496 | 0.463 |
| HYPE | 1.0 | 12 | +0.032 | +0.012 | 0.516 | 0.475 |
| HYPE | 2.0 | 13 | −0.085 | −0.029 | 0.460 | 0.418 |

## Findings

**1. No cell clears DSR > 0.95.** Best in the whole sweep is HYPE θ=1.0
at DSR=0.475 — coin-flip. Nothing is statistically deployable. The
significance layer correctly refuses to bless any cell.

**2. OFI cancel does not consistently help; on APE it backfires.**
APE off (−0.24) → θ=1.0 (−2.28) is a 9× worse loss. The cancellation
leaves one-sided inventory that costs more to unwind than the adverse
fills it avoids. The per-fill markout uplift from the sim is real but
swamped by inventory + queue second-order effects the sim didn't model.

**3. APE full-day is net negative (−$0.24 baseline)** — the earlier
optimistic +$0.36 single-hour figure (hour-00) was a lucky tail.
Confirmed by both this sweep and the 2026-05-26 full-N APE sweep.

**4. Fill counts remain thin** (XMR 9/day, HYPE 13/day, APE 51/day).
Even pooling 24 hours, per-cell N is 3-51 — far below what DSR needs to
reach significance regardless of the underlying edge. The constraint is
as much data-starvation as signal-starvation.

## Implications

- **OFI cancel as built is not a deployable edge** on HL perps. Shelve
  the live AS port (the C++ rule stays in the codebase, disabled by
  default via the inf sentinel — costs nothing to keep).
- **The strategy, not the rule, is the problem.** Baseline AS is
  ~break-even-to-negative on every instrument tested. No suppression
  tweak rescues a strategy with no underlying edge. Future work should
  question the AS spread/sizing fundamentals or the instrument universe,
  not add more defenses.
- **Data scale is a hard blocker for significance.** To get any cell to
  DSR > 0.95 we need either far more fills (more days, or higher fill-rate
  instruments) or a genuinely stronger signal. One day of 3 thin
  instruments can't clear the bar.

## Caveats

1. One day (2026-05-07). Regime-specific. A multi-day sweep might shift
   the picture but is unlikely to manufacture significance at these fill rates.
2. APE sized at $80 (qty 500) here vs $8 (qty 50) in the original OFI
   cancel arc — not directly comparable to those numbers.
3. Spread floor is APE-tuned (min_half_spread_bps=2.0) for all coins;
   per-instrument spread calibration not done. Tighter floors might lift
   fill counts (and change the conclusion) on XMR/HYPE.

## Reproduce

```bash
bash /tmp/sweep_multi_ofi.sh        # 288 cells, ~10 min
python3 /tmp/agg_multi_ofi.py       # PSR/DSR table
```
