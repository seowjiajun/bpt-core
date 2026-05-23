# Ridge regression on per-instrument composite — second negative result

**Date:** 2026-05-23
**Author:** follow-up to `2026-05-23_composite_gate_negative.md`
**Status:** Complete — ridge fails the same way the gate did. Per-instrument OLS is the practical ceiling on this data.

## Question

The single-feature gate ruled out **sparsity** (all-or-nothing weight zeroing) as a fix for the 35.7% residual loss in the per-instrument composite. The natural follow-up: does **proportional shrinkage** (ridge) help where sparsity didn't?

Hypothesis: OLS slightly over-fits the smaller weight on OFI-dominant instruments. Ridge shrinks both weights proportionally — the dominant weight is barely affected, but the smaller weight pulls toward zero gently. This should keep the useful information at a calibrated level while damping the train-set noise.

## Setup

| | |
|---|---|
| Data | Same 10 HL hour-00 canon days, 7-train / 3-test split |
| Universe | 182 instruments |
| Model | Per-instrument ridge: `ret_1s = β_OFI · z(ofi) + β_MP · z(microprice_dev) + α`, fit by minimising `‖y - Xβ‖² + λ(β_OFI² + β_MP²)` — penalty on slopes only, not intercept |
| λ sweep | {0, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7}. λ=0 is OLS baseline. |
| Script | `bpt-research/experiments/ofi_composite_ridge_sweep.py` |

## Result — ridge monotonically hurts (very gently)

| λ | Mean OOS IC | Median IC | Win rate vs best-single |
|---|---|---|---|
| **0 (OLS)** | **0.1566** | 0.1462 | **64.3%** |
| 1e2 | 0.1566 | 0.1462 | 64.3% |
| 1e3 | 0.1566 | 0.1463 | 63.7% |
| 1e4 | 0.1565 | 0.1469 | 64.3% |
| 1e5 | 0.1559 | 0.1484 | 63.7% |
| 1e6 | 0.1555 | 0.1486 | 63.2% |
| 1e7 | 0.1554 | 0.1486 | 63.2% |

Effectively flat from λ=0 → λ=1e4 (weights barely shrunk), then gentle degradation as λ goes higher. **Best λ is 0** — plain OLS wins, with margin only growing as λ climbs.

The interesting secondary observation: median IC *rises* with λ even while mean IC drops. So ridge **helps the middling instruments slightly** (those nearest the median) while hurting the top-IC instruments (where OLS's weight magnitudes are doing real work). On balance, the top hurt outweighs the middle help — the mean loses.

## Why both gate AND ridge failed

The gate (sparsity) tested: *"is the small weight noise?"* — and the answer was no.

Ridge (shrinkage) tested: *"is OLS slightly over-confident on the small weight?"* — and the answer is also no, or at least the over-confidence is so mild that any correction costs more than it saves.

Both being negative pins down what the 35.7% residual loss actually is:

- **Not a small-weight-noise problem** (gate would have helped)
- **Not a slight-over-fit-of-magnitude problem** (ridge would have helped)
- → Most likely: **genuine train/test regime drift on those instruments**, OR **OLS doesn't have enough relevant features to fit** (an information problem, not a weight-calibration problem)

## Implications

1. **Per-instrument OLS is the practical ceiling on this data with these two features.** No post-fit polish (sparsity, shrinkage) materially improves things.
2. **The only remaining lever is the information set.** Adding a third feature with low correlation to both OFI and microprice-dev (Spearman +0.33 at cell level — already too overlapping for one to add much to the other) would meaningfully expand the regression's information.
3. **Two-feature regressions are exhausted.** Stop sweeping post-fit knobs on (OFI, microprice-dev); move on to broader feature exploration.

## Caveats

1. **Two features only.** With 3+ features the regularization story could change — ridge might dampen genuine multicollinearity between, say, OFI and queue-imbalance, where it can't between OFI and microprice (these are too directly related for ridge to disambiguate).
2. **Per-cell z-scoring** is what makes ridge's λ comparable across instruments. Without it the units of `β_OFI` and `β_MP` would differ by ~10⁴ and λ would have to be tuned per-instrument.
3. **No nested cross-validation** for λ. Single train/test split. λ could be over-fit on test in principle; in practice the curve is so flat at the OLS end that this doesn't materially affect the conclusion.
4. Same data caveats as the parent docs (hour-00 UTC, single venue, no fees).

## Next experiments (highest information value first — revised)

1. **Add a third feature**: queue imbalance, trade-flow EWMA, order-arrival rate. The information set is the bottleneck; everything else has been ruled out.
2. **Weight stability check** (k-fold per-instrument weights). Tells us whether the 35.7% residual loss is *also* explained by training-window variance — if AAVE's weights swing wildly across 7-day windows, more training data is the answer.
3. **Hour-of-day cut** — still on the parent backlog. Possibly relevant to "regime drift" hypothesis.

## Reproduce

```bash
LD_PRELOAD=/lib/x86_64-linux-gnu/libstdc++.so.6 \
  python bpt-research/experiments/ofi_composite_ridge_sweep.py
# output: /tmp/bpt_canon/composite_ridge_sweep{,_summary}.csv
```
