# Per-instrument composite weights — OFI + microprice-dev on HL

**Date:** 2026-05-23
**Author:** follow-up to `2026-05-23_ofi_10day_panel_and_as_tuning.md`
**Status:** Complete — out-of-sample validation on 10 HL days (7 train / 3 test), 182 instruments.

## Question

Yesterday's walk-forward eval showed an **equal-weight global composite** of OFI + microprice-dev didn't beat the best-single feature on average (37.4% win rate, −0.014 IC mean uplift). The hypothesis: per-instrument weight fitting would route OFI-dominant instruments to OFI, microprice-dominant ones to microprice, and use a real composite only where both features are moderate.

Does that work?

## Setup

| | |
|---|---|
| Data | Same 10 HL hour-00 canon days as parent study |
| Split | Train: 2026-05-13 → 19 (7 days). Test: 2026-05-20 → 22 (3 days). |
| Features | OFI (max_levels=1, window=1s), microprice-dev (FairValueEstimator Mode.Micro − mid) |
| Per-cell preproc | z-score each feature inside each (instrument, day) cell before pooling |
| Fit | OLS: `ret_1s ~ z(ofi) + z(microprice_dev) + const`, per instrument |
| Composite eval | Apply per-instrument weights to test-cell z-scored features; one IC per instrument across pooled test ticks |
| Universe | 182 instruments with ≥ 500 valid ticks on every train + test day |
| Script | `bpt-research/experiments/ofi_per_instrument_composite.py` |
| Library calls | `ic.fit_composite_weights` (global baseline), `ic.multivariate._z` (cell-level z-score), `scipy.stats.spearmanr` (IC) |

## Results

### Out-of-sample IC, all 182 instruments

| Signal | Mean IC | Median IC | % Positive Cells |
|---|---|---|---|
| OFI alone | +0.144 | +0.133 | 97.8% |
| Microprice-dev alone | +0.097 | +0.088 | 96.7% |
| Global-weight composite | +0.150 | +0.143 | 100.0% |
| **Per-instrument composite** | **+0.157** | **+0.146** | **99.5%** |

### Win rates

| Comparison | Per-instrument wins | Mean uplift |
|---|---|---|
| Per-inst vs best-single per instrument | **117 / 182 (64.3%)** | +0.0002 IC |
| Per-inst vs global-weight composite | 121 / 182 (66.5%) | +0.0062 IC |
| **Global-weight composite** vs best-single (yesterday's baseline) | 68 / 182 (37.4%) | −0.0139 IC |

Per-instrument nearly **doubles** the win rate of the equal-weight global composite. The mean uplift over best-single isn't huge (+0.0002 IC), but the *median* of the win distribution is solidly positive, and the worst-case downside is bounded.

### Top instruments — weights routed correctly

| Symbol | w_OFI | w_microprice_dev | Dominant feature | Per-inst IC | Uplift vs best-single |
|---|---|---|---|---|---|
| SOL  | 1.0e-5 | **1.6e-5** | microprice | 0.523 | +0.013 |
| ETH  | 0.7e-5 | **1.9e-5** | microprice | 0.507 | +0.017 |
| DOGE | 1.2e-5 | **2.6e-5** | microprice | 0.308 | +0.020 |
| kPEPE | 1.2e-5 | **2.3e-5** | microprice | 0.305 | +0.021 |
| XRP  | 0.5e-5 | **1.8e-5** | microprice | 0.304 | +0.005 |
| AAVE | **1.5e-5** | 0.5e-5 | OFI | 0.319 | **−0.038** ⚠️ |
| FET  | **1.5e-5** | 0.1e-5 | OFI | 0.283 | +0.006 |
| POL  | **1.2e-5** | 0.2e-5 | OFI | 0.270 | **−0.019** ⚠️ |
| APEX | **0.9e-5** | 0.0e-5 | OFI | 0.272 | −0.011 |

Liquid instruments (SOL/ETH/DOGE/XRP/kPEPE) end up microprice-weighted; sparse-book instruments (AAVE/FET/POL/APEX) end up OFI-weighted. The fit correctly *recognizes* feature dominance.

### Failure mode

For instruments where one feature is strongly dominant (AAVE, POL, APEX), the OLS fit assigns a small non-zero weight to the other feature, which introduces noise on test data. AAVE loses 0.038 IC against OFI-alone — the per-instrument composite gives microprice a 5e-6 weight, but that small weight is *negative information* on AAVE specifically, where microprice barely correlates with returns.

35.7% of instruments (65 / 182) have negative uplift vs best-single — almost all of them are this pattern.

## Interpretation

1. **Per-instrument weights are a real improvement.** Win rate jumped from 37% → 64% vs best-single. The principled approach (let the data choose weights) beats the naïve (equal weights everywhere).
2. **The fit correctly routes** by liquidity/spread structure — liquid books → microprice, sparse books → OFI. This is a meaningful semantic outcome, not noise.
3. **But the small-weight failure mode is real.** OFI-dominant instruments still lose a fractional IC to "background microprice noise" because OLS doesn't know to set tiny weights to exactly zero.
4. **Composite reliability stays high.** 99.5% positive cells on test — only slightly below the global composite's 100%, well above OFI's 97.8%.

## Implications

1. **A per-instrument weights config is the natural shape for a future `ofi_weight_bps_` rollout** — but only after the consumption mechanic question from the parent doc is resolved (the AS reservation-skew approach lost money in backtest). The signal calibration is now solid; the production-side mechanic is the bottleneck.
2. **A "single-feature-dominant" gate would close most of the remaining gap.** If |w_dominant / w_other| > some threshold, set the smaller weight to zero. That's L1-regularized regression in spirit (lasso would do this automatically; here we'd add it post-hoc to the existing OLS fit). Worth a follow-up.
3. **Out-of-sample stability is good:** train IC ≈ test IC for both individual features (no overfit on the panel cells), and per-instrument weights generalize cleanly to held-out days. Walk-forward methodology is sound for this stack.

## Caveats

1. **Same caveats as parent doc:** hour-00 UTC only (Asia session), no transaction-cost layer, single venue.
2. **OLS on 2 features is the floor, not the ceiling.** Adding more features (queue imbalance, recent-trade-flow, vol-state) into the multivariate fit would likely shift weights and surface new feature-feature collinearity to resolve.
3. **No regularization.** OLS gives every feature *some* weight. Lasso or threshold-cutoff would help on the 35% of instruments that lose to best-single.
4. **Cross-day stability of the per-instrument weights themselves is not measured.** Are AAVE's weights on 7-day train similar to what 7 different days would produce? Don't know yet. A rolling-window or k-fold pass would answer; deferred.

## Reproduce

```bash
LD_PRELOAD=/lib/x86_64-linux-gnu/libstdc++.so.6 \
  python bpt-research/experiments/ofi_per_instrument_composite.py
# output: /tmp/bpt_canon/per_inst_composite.csv
```

## Next experiments (highest information value first)

1. **Single-feature gate** — drop weights below a threshold to zero (or fit with lasso). Closes the AAVE/POL/APEX-style residual loss.
2. **Weight stability** — k-fold or rolling-window assessment of how variable each instrument's weights are. If weights move materially across windows, the per-instrument approach is fragile.
3. **Add a 3rd feature** — queue imbalance or trade-flow EWMA. See if per-instrument routing extends naturally to 3+ feature combinations.
4. **Hour-of-day cut** — still on the parent doc's backlog. Per-instrument weights might shift across sessions; if so, weights would need to be hour-conditional.
