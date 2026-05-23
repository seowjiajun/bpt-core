# Single-feature gate on per-instrument composite — negative result

**Date:** 2026-05-23
**Author:** follow-up to `2026-05-23_per_instrument_composite.md`
**Status:** Complete — clean negative finding. Sparsity gate doesn't help; OLS is already near-optimal.

## Question

Yesterday's per-instrument composite (OFI + microprice_dev with OLS-fitted per-instrument weights) lifted OOS mean IC to +0.157 but **35.7% of instruments still lost a fractional IC to best-single**. The dominant pattern: OFI-strong instruments (AAVE, POL, APEX) got a small non-zero microprice weight that *looked* like noise.

The hypothesis: a **single-feature gate** that zeros out the smaller weight when |w_dominant / w_other| exceeds some threshold would close that residual loss.

Does it?

## Setup

| | |
|---|---|
| Data | Same 10 HL hour-00 canon days, 7-train / 3-test split |
| Universe | 182 instruments (same as parent study) |
| Per-instrument weights | OLS fit identical to parent study; cached, then gated post-hoc |
| Gate rule | If `|w_dominant| / |w_other| > T`, set `w_other = 0`. T=∞ = no gate (parent baseline). T=1 = always single-feature. |
| Thresholds swept | {1.0, 1.5, 2.0, 3.0, 5.0, 10.0, ∞} |
| Script | `bpt-research/experiments/ofi_composite_gate_sweep.py` |

## Result — gating monotonically *hurts* mean IC

| Threshold | Mean OOS IC | Median IC | % Positive | Wins vs best-single | Instruments gated |
|---|---|---|---|---|---|
| 1.0 (force single feature) | 0.1458 | 0.1314 | 97.8% | 25 / 182 (14%) | 182 |
| 1.5 | 0.1486 | 0.1367 | 97.8% | 39 / 182 (21%) | 145 |
| 2.0 | 0.1516 | 0.1428 | 98.4% | 58 / 182 (32%) | 109 |
| 3.0 | 0.1537 | 0.1439 | 98.9% | 78 / 182 (43%) | 69 |
| 5.0 | 0.1558 | 0.1439 | 98.9% | 105 / 182 (58%) | 26 |
| 10.0 | 0.1564 | 0.1446 | 99.5% | 113 / 182 (62%) | 12 |
| **∞ (no gate)** | **0.1566** | **0.1462** | **99.5%** | **117 / 182 (64.3%)** | **0** |

Strictly monotonic: every level of gating reduces IC. **The non-dominant feature's small weight isn't noise — it carries usable information on test data, just less than the dominant feature.** Zeroing it removes signal, not noise.

## Why the hypothesis was wrong

Two possible reasons for the AAVE-style residual loss vs best-single:

1. **"Small weight = noise" (the hypothesis we tested).** If true, gating would help.
2. **OLS gives slightly suboptimal absolute weights on test data due to train/test regime drift, but feature *selection* is correct.** If true, gating doesn't help — you'd need shrinkage (ridge), not sparsity (lasso/gate).

The sweep result rules out (1) and points to (2). The OLS fit picks the right features but slightly over-weights the smaller one on instruments where one feature is dominant; gating to zero overcorrects.

## Implications

1. **Per-instrument OLS is the practical ceiling on this data with these two features.** Further IC uplift will require a different lever, not better post-processing of the same weights.
2. **The remaining 35.7% residual loss is structural** — likely a mix of regime drift between train and test, and OLS's intrinsic finite-sample noise. Neither is addressable by sparsity.

## Levers worth trying instead (highest information value first)

1. **Ridge regression** — shrinks all weights proportionally toward zero (vs the all-or-nothing gate). On AAVE, the microprice weight would shrink from 5e-6 to maybe 2e-6 instead of dropping to 0 entirely — keeps the useful information at a calibrated level. Cheap follow-up; `ic.fit_composite_weights` would gain a `lambda_` parameter.
2. **Third feature** — the residual loss might just need an additional predictor (queue imbalance, trade-flow EWMA) that complements OFI without overlapping it as much as microprice does. Multi-feature routing extends naturally to 3+ inputs.
3. **Larger training window** — 7 days may be enough for the headline mean but not enough to fit *per-instrument* weights with low variance. A rolling-window stability check (across multiple 7-day windows) would tell us how shaky each per-instrument weight is. If shaky, more data is the answer.

## Caveats

1. Same data caveats as the parent docs (hour-00 UTC, single venue, no transaction-cost layer).
2. **Tested only one gate flavor** (absolute-magnitude ratio). A statistical-significance-based gate (drop weight if its OLS standard error overlaps zero) might pick a different cut. Not tested.
3. Two features only. With 3+ features, the gate question becomes more interesting (drop *which* of several weak features?) — would deserve re-testing.

## Reproduce

```bash
LD_PRELOAD=/lib/x86_64-linux-gnu/libstdc++.so.6 \
  python bpt-research/experiments/ofi_composite_gate_sweep.py
# output: /tmp/bpt_canon/composite_gate_sweep{,_summary}.csv
```
