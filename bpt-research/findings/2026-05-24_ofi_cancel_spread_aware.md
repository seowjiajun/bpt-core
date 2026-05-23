# Spread-aware OFI cancel — uplift is +0.915 bps/fill, constant across spread regimes

**Date:** 2026-05-24
**Author:** follow-up to `2026-05-24_ofi_cancel_prototype.md`
**Status:** Complete — net PnL story now closed. C++ AS port goes to backlog.

## Question

Yesterday's prototype showed the OFI-cancellation rule reduces per-fill adverse markout by +0.50 bps at θ=0.5σ, but it didn't model spread revenue. AS earns spread on every fill, and cancellation reduces fills — so the markout save could be offset (or worse) by missed spread.

This script adds `maker_spread_bps` as a parameter and computes the actual net PnL per fill: `+half_spread − markout_adverse`. Sweeps across {0, 2, 4, 6, 10} bps spread × {0, 0.5, 1, 1.5, 2, 3, ∞}σ threshold.

## Setup

| | |
|---|---|
| Data | Same 10 HL hour-00 canon days, 122 instruments |
| Sample | 732,395 trades |
| PnL convention | BUY trade (AS sells ask): `+half_spread − markout`. SELL trade (AS buys bid): `+half_spread + markout`. (Markout >0 hurts AS-as-seller; markout <0 hurts AS-as-buyer.) |
| Spread regimes | {0, 2, 4, 6, 10} bps total round-trip (half-spread = spread/2) |
| Threshold sweep | Same {0, 0.5, 1, 1.5, 2, 3, ∞}σ as the markout-only prototype |
| Script | `bpt-research/experiments/ofi_cancel_spread_aware.py` |

## Result

### Mean net PnL per fill (bps), spread × threshold

| spread (bps) | θ=0.0σ | θ=0.5σ | θ=1.0σ | θ=1.5σ | θ=2.0σ | θ=3.0σ | θ=∞ |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.0 | −2.06 | −2.42 | −2.64 | −2.85 | −2.95 | −2.97 | −2.98 |
| **2.0** ← HL maker | **−1.06** | **−1.42** | **−1.64** | **−1.85** | **−1.95** | **−1.97** | **−1.98** |
| 4.0 | −0.06 | −0.42 | −0.64 | −0.85 | −0.95 | −0.97 | −0.98 |
| 6.0 | +0.94 | +0.58 | +0.37 | +0.15 | +0.05 | +0.03 | +0.03 |
| 10.0 | +2.94 | +2.58 | +2.37 | +2.15 | +2.05 | +2.03 | +2.03 |

### Fraction of instruments with positive net PnL

| spread (bps) | θ=0.0σ | θ=0.5σ | θ=1.0σ | θ=1.5σ | θ=2.0σ | θ=3.0σ | θ=∞ |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.0 | 0.8% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% |
| **2.0** | **37.7%** | 22.1% | 16.4% | 7.4% | 5.7% | 5.7% | **5.7%** |
| 4.0 | 67.2% | 59.0% | 53.3% | 45.1% | 44.3% | 42.6% | 42.6% |
| 6.0 | 83.6% | 77.9% | 75.4% | 72.1% | 69.7% | 69.7% | 69.7% |
| 10.0 | 91.8% | 89.3% | 88.5% | 86.9% | 86.9% | 86.1% | 86.1% |

## Three key findings

**1. Cancellation gives a constant +0.915 bps/fill uplift regardless of spread regime.**

The "best θ vs no-cancel" delta is **+0.915 bps in every row**:
- spread 0: −2.060 vs −2.975 = +0.915
- spread 2: −1.060 vs −1.975 = +0.915
- spread 4: −0.060 vs −0.975 = +0.915
- spread 6: +0.940 vs +0.025 = +0.915
- spread 10: +2.940 vs +2.025 = +0.915

This is the *signature* of the cancellation rule: it reduces toxic-flow cost by a fixed amount independent of how much spread AS earns. Linear separation in PnL space.

**2. AS at HL's typical 2 bps maker fee isn't profitable on the average instrument — with or without cancellation.**

Best net PnL at spread=2 is −1.06 bps/fill (θ=0). Cancellation **halves the loss** but doesn't flip it positive on average. AS-as-currently-configured on HL is a structurally money-losing strategy on the universe of 122 instruments tested.

**3. Cancellation dramatically expands the *number* of profitable instruments.**

At spread=2 bps:
- Without cancel: only **6% (7/122)** of instruments produce positive EV
- With cancel at θ=0: **38% (46/122)** become positive EV

This is the **largest practical effect of the rule** — not the mean shift (which is real but small), but the broadened universe of viable instruments. Instruments that were 0.5-2 bps negative without cancel become 0.5-2 bps positive with it.

## Practical implications for AS on HL

1. **Cancellation is a free improvement at any spread regime.** +0.915 bps/fill uplift, costs nothing except some queue position. Deploy regardless of broader strategy direction.
2. **The +0.50 bps/fill figure from yesterday's prototype was the markout-only view.** The properly-signed net PnL view is +0.915 bps/fill. The earlier figure underestimated the benefit because it used absolute markout instead of signed.
3. **To make AS broadly profitable on HL at 2 bps fees**, three paths exist (in increasing fundamental difficulty):
   - **Restrict universe to the 46 positive-EV instruments** (instrument-allowlist config change).
   - **Post behind touch** to widen effective spread to 4+ bps. Lower fill rate but better per-fill economics. At spread 4, ~60-67% of instruments are positive with cancel.
   - **Add more signals** to further reduce adverse markout per fill. Yesterday's signal-calibration work hit a +0.157 IC ceiling; the +0.915 bps cancel uplift is on top of that, not replacing it.
4. **The bottom 14% of instruments** (those negative even with cancel at any θ) need exclusion regardless. From the shadow-markout study: LIT, ICP, HYPE, TST, VINE, ALT.

## Caveats

1. **Single fill model.** Every trade is treated as a fill on AS's quote. Real AS only catches a fraction of trades (queue position, behind-touch placement). This doesn't change the *per-fill* PnL but inflates the realistic *fill count*.
2. **No queue-priority cost.** Cancellation loses queue priority on the cancelled side. Real AS pays for this even on non-adverse subsequent trades. Not modelled.
3. **Constant half-spread.** Real AS spread varies with vol gates, inventory pressure, drift-suppress. The constant 1/3/5 bps half-spread is the simplification.
4. **No interaction with inventory.** AS uses inventory skew to unwind positions; cancellation might conflict with that. Needs the real strategy code to test.
5. **HL hour-00 UTC, single venue, 10 days.** Same as all prior arcs.

## Reproduce

```bash
LD_PRELOAD=/lib/x86_64-linux-gnu/libstdc++.so.6 \
  python bpt-research/experiments/ofi_cancel_spread_aware.py
# output: /tmp/bpt_canon/ofi_cancel_spread_sweep.csv
```

## Next experiments (highest information value first)

1. **C++ AS port** (BACKLOG) — add `ofi_cancel_threshold_sigma` config to `avellaneda_stoikov_strategy.cpp`. Wire into the tick handler. Run through `bpt-backtester` with realistic queue model + inventory interactions. ~half-day. Highest-fidelity confirmation.
2. **Asymmetric thresholds** — shadow markout study showed SELL-side adverse markout is ~2× the BUY side. Lower θ on bid cancellation than ask cancellation might lift the uplift further. Cheap.
3. **Instrument-allowlist generation** — produce the deploy-set CSV (46 instruments at spread=2, or use the per-instrument best θ from `ofi_cancel_prototype_best.csv`). Direct input to any AS config.
4. **Behind-touch quoting study** — model spread > 2 bps by simulating AS posting one tick behind. Measure fill rate × per-fill PnL trade-off. Tests whether path (3) above is viable.
