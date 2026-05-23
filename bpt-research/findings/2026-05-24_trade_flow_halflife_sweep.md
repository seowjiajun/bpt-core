# Trade-flow halflife sweep — no halflife saves it

**Date:** 2026-05-24
**Author:** addendum to `2026-05-24_3feature_composite.md`
**Status:** Complete — second clean negative on trade_flow_ewma. Feature exhausted; pivot to a different 3rd candidate.

## Question

Yesterday's 3-feature composite (OFI + microprice + trade_flow_ewma at halflife=1s) tied the 2-feature baseline on mean OOS IC. Trade-flow met the orthogonality criterion (pairwise Spearman ~0.05 with both OFI and microprice) but failed the strength criterion (standalone median IC +0.007).

Hypothesis: the 1s halflife was an unmotivated guess. Maybe a different halflife produces a materially stronger standalone signal, which would change the 3-feature uplift story.

## Setup

Same 7-train / 3-test split, 182 instruments. Per halflife `hl ∈ {0.1, 0.5, 1, 5, 30, 300}`:

1. Recompute `trade_flow_ewma(hl)` per (instrument, day) cell
2. Standalone median IC + pos-frac (% of instruments with positive IC)
3. Pairwise Spearman with OFI and microprice (pooled across train)
4. Per-instrument 2-feature and 3-feature composite OOS IC
5. Win rate of 3-feat vs 2-feat

Script: `bpt-research/experiments/trade_flow_halflife_sweep.py`

## Result

| halflife (s) | corr w/ OFI | corr w/ MP | med IC | pos_frac | 3-feat IC | 2-feat IC | 3v2 win % |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.1   | +0.066 | +0.039 | +0.0041 | 58.8% | **+0.1567** | +0.1566 | 44.5% |
| 0.5   | +0.064 | +0.038 | +0.0060 | 59.3% | **+0.1567** | +0.1566 | 45.1% |
| 1.0   | +0.060 | +0.036 | +0.0070 | 62.6% | +0.1566 | +0.1566 | 47.3% |
| 5.0   | +0.044 | +0.028 | **+0.0078** | **66.5%** | +0.1560 | +0.1566 | 39.0% |
| 30.0  | +0.024 | +0.012 | +0.0037 | 58.8% | +0.1554 | +0.1566 | 39.6% |
| 300.0 | +0.014 | +0.012 | +0.0022 | 57.1% | +0.1557 | +0.1566 | 34.6% |

**Best 3-feature uplift over 2-feature: +0.0001 IC (at hl=0.5s).** Effectively zero. Win rate never crosses 50%.

## What the sweep reveals

Two trade-offs are visible across halflives:

1. **Standalone IC and pos-frac peak at hl=5s** (median +0.008, 66.5% positive). The middle halflife has the strongest *individual* trade-flow signal.
2. **Cross-feature correlations decrease with longer halflives.** Short halflives put trade-flow closer to OFI's resting-side imbalance (corr 0.066); long halflives smooth it out enough that the correlations drop toward zero.

**The composite picks the worst of both worlds.** Short halflife: weak standalone IC, but OLS gives it a tiny weight because it's correlated with OFI — does no harm. Long halflife: slightly stronger standalone IC, but uncorrelated enough that OLS gives it more weight — and that weight on a still-weak feature introduces test-time noise that costs the composite slightly. The 3-feature composite IC monotonically declines past hl=1s.

## Interpretation

Trade-flow EWMA, at any halflife tested, doesn't carry enough information *as a standalone signal* to lift a multi-feature composite. The orthogonality is real but unproductive — confirms the earlier observation that orthogonality is necessary but not sufficient.

Two related conclusions:

1. **Trade-flow as currently formulated is exhausted.** No more knobs on this feature are worth turning. The asymmetry-of-aggressor signal is just not strong enough on 1s forward returns at HL hour-00.
2. **The 5s halflife being "strongest standalone but worst composite" is the diagnostic finding.** It tells us trade-flow's marginal signal is mostly redundant with what's *already* in the (OFI, microprice) pair — exactly when the standalone IC is highest. The right 3rd feature won't have this dynamic; it'll be strong standalone AND uncorrelated.

## Implications

1. **Pivot to a different 3rd feature.** Trade-flow is done.
2. **Stronger candidates:**
   - **Microprice momentum** (∂microprice/∂t over a short window) — derivative of book-state evolution, distinct mechanism from level-snapshot microprice or window-summary OFI.
   - **Spread innovation** (recent Δ(ask − bid)) — captures *market-maker confidence shifts*, likely uncorrelated with all three.
   - **Trade arrival rate** (count of trades per second, unsigned) — captures *activity bursts*, distinct from *directional flow*.
3. **The per-instrument 2-feature composite remains the operating point** — +0.157 mean OOS IC, 64% win vs best-single, 99.5% positive cells. Until a better 3rd feature shows up, that's the calibration ceiling.

## Caveats

1. **Single per-tick aggregation.** Could try volume-time bars instead of clock-time EWMA (sign+volume on a per-trade basis, not per-second). Different shape; not tested.
2. **Single venue, single hour.** HL hour-00 only — same as all prior arcs. Trade-flow may be a stronger signal at different times of day or on different venues.
3. **No nested CV on halflife** — single 7/3 split. Standard caveat. If the best halflife had been hl=1.0 (the original), we might still suspect overfitting; given the result is "no halflife works," overfitting isn't the concern.

## Reproduce

```bash
LD_PRELOAD=/lib/x86_64-linux-gnu/libstdc++.so.6 \
  python bpt-research/experiments/trade_flow_halflife_sweep.py
# output: /tmp/bpt_canon/trade_flow_halflife_sweep.csv
```
