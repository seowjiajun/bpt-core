# OFI predictiveness on Hyperliquid (replicated across 2 days, 10 instruments)

**Date:** 2026-05-23
**Author:** validation run during research-stack bring-up
**Status:** preliminary — 2 days of data, single venue. Worth pursuing.

## Question

Does the Order Flow Imbalance (OFI) feature — same C++ implementation
AS uses for its reservation skew — predict forward mid returns on
Hyperliquid?

## Setup

| | |
|---|---|
| Data | HL public-mainnet WS captures from `bpt-tape`, converted via `bpt-canon-replay` |
| Days | 2026-04-25 (78 MB wslog, ~4.5k BBO ticks/instrument), 2026-05-06 hour 00 (130 MB, ~6.7k BBO ticks/instrument) |
| Instruments | 10 in common (ETH, SOL, APE, HYPE, ZEC, AAVE, DOGE, TAO, FARTCOIN, kPEPE) |
| OFI config | `max_levels=1` (BBO only), `window_ns=1s` — same defaults AS uses |
| Forward returns | `log(mid_{t+h} / mid_t)` for h ∈ {100 ms, 1 s, 10 s} via `pd.merge_asof(direction='forward')` |
| Score | Spearman rank correlation (Information Coefficient) |

Pipeline: `canon → bpt_canon.read_bbos() → bpt_features.OFICalculator (pybind11) → forward returns → spearmanr`. Same C++ runs in production and research — no implementation drift.

## Result

IC at 1 second horizon, per instrument, both days:

| Instrument | 2026-04-25 IC@1s | 2026-05-06 IC@1s |
|---|---|---|
| ETH      | 0.202 | 0.344 |
| SOL      | 0.287 | 0.354 |
| APE      | 0.258 | 0.187 |
| HYPE     | 0.115 | 0.196 |
| ZEC      | 0.124 | 0.111 |
| AAVE     | 0.234 | 0.316 |
| DOGE     | 0.194 | 0.268 |
| TAO      | 0.125 | 0.279 |
| FARTCOIN | 0.103 | 0.160 |
| kPEPE    | 0.243 | 0.260 |

**Headline numbers**:
- 10/10 instruments positive on both days. Cross-day sign-flip rate: 0.
- IC range across panel × days: 0.10 – 0.36, mean ≈ 0.22.
- Magnitude is higher on 2026-05-06 across most instruments (denser data + likely more directional regime).

Per-horizon decay (averaged across instruments):

```
horizon   |  mean |IC| across instruments × days
  100ms   ≈ 0.22
   1s     ≈ 0.22
  10s     ≈ 0.12
```

Signal lives at sub-second to single-second horizons. Decays by 10 s.

## Interpretation

This is the textbook OFI / book-pressure pattern: positive IC, short-horizon directional, decays as market makers replenish. The signal is **not** a single-instrument or single-day fluke — it replicates across 10 instruments × 2 days, all positive at 1 s.

The IC magnitude (0.10–0.36) is high vs equity microstructure norms (typically 0.02–0.05 for OFI on liquid US equities) but plausible for crypto perpetuals on a less-competitive venue. Hyperliquid's onchain matching + visible book gives flow signals less HFT competition to crowd out.

## Caveats

1. **Two days of data.** Need ≥10 days across regimes (trending / chop / vol-spike) before any production-direction decision.
2. **Single venue.** OFI on OKX BTC-USDT-SWAP would likely be 0.05–0.10, not 0.20 — replicate on OKX archive before generalising.
3. **No transaction-cost layer.** IC says "OFI's direction predicts forward return direction." It does NOT say "you can profit by trading on it." Maker fees, taker fees, slippage, queue position all subtract from IC's apparent edge.
4. **OFI is partly autocorrelated with recent returns.** The signal it carries is genuinely directional but not *new* information at every tick — it's a smoothed reflection of recent order flow. A strategy using it has to size for "incremental signal beyond what's already in the price."
5. **`merge_asof(direction='forward')` semantics:** the forward mid is from the first row with `ts_ns ≥ t + horizon`. Strictly future. No lookahead bug — manually verified.

## Implications

1. **Validates AS's existing OFI usage** (`ofi_weight_bps_` param). AS feeds OFI into its reservation-price skew. This finding is evidence the input is genuinely informative, not just decorative.
2. **A pure-OFI taker strategy on HL is worth a sketch.** Even after fees, 0.20 IC at 1s is the kind of edge taker strategies are built on. Estimate: maker_bps=2, taker_bps=5 → need post-cost edge ≥7 bps. Current OFI signal at 1s of 0.20 IC translates roughly to a 10-15 bp expected move per σ-sized OFI deviation — plausibly above cost on the larger panel (ETH, SOL, AAVE).
3. **Cross-instrument signal averaging** is the natural next experiment — combine OFI from BTC + ETH + SOL into a single directional signal for each, see if diversification raises IC further.

## Reproduce

```bash
# 1. Convert wslog → canon (if not already done)
bazel run //bpt-canon:bpt-canon-replay -- \
  --wslog /opt/bpt/data/raw/hyperliquid/2026-04-25/hyperliquid-095045.wslog \
  --instrument-mapping config/instruments/instrument_mapping.hyperliquid-mainnet.json \
  --output /tmp/bpt_canon/hl-2026-04-25.canon

# Same for 2026-05-06/hyperliquid-004026.wslog →
#   /tmp/bpt_canon/hl-2026-05-06-h00.canon

# 2. Setup once (see bpt-research/README.md)
bazel build //bpt-features/python/bpt_features:_core
ln -sf "$(pwd)/bazel-bin/bpt-features/python/bpt_features/_core.so" \
       bpt-features/python/bpt_features/_core.so

# 3. Notebook
LD_PRELOAD=/lib/x86_64-linux-gnu/libstdc++.so.6 \
  jupyter nbconvert --to notebook --execute --inplace \
  bpt-research/notebooks/02_feature_predictiveness.ipynb
```

## Next experiments (highest information value first)

1. **Cross-venue replication** — ingest 1 day of OKX BTC-USDT-SWAP, re-run the per-instrument IC. If OFI IC on BTC ≈ 0.05–0.10, the HL number is real but venue-amplified. If ≈ 0, something venue-specific to HL is going on.
2. **Cross-day stability** — extend to 10 HL days. Compute IC's own variance across days per instrument. The instruments with the lowest cross-day IC variance are the best candidates for a strategy.
3. **Conditional IC** — slice by realized-vol decile, regime tag, time of day. Find the regime where OFI IC is highest — that's where the strategy should be on, off in other regimes.
4. **Post-fee P&L simulation** — convert the IC into expected returns net of HL maker/taker fees + slippage. The translation from IC → P&L is non-trivial; needs a strategy spec, not just the signal.
