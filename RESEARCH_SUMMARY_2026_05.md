# Research Summary — 2026-05

State of the trading-system research as of the end of this session. Future sessions should start here.

---

## TL;DR

- Brought the simulator to HFT-grade across **7 phases**: intra-day windowing, determinism + multi-window, latency model + sessions, sweep + walk-forward + cell isolation, queue regen on cancels, validation harness, pre-prod gates.
- Built a complete pre-prod gate toolchain: `sweep.py`, `validate.py`, `sensitivity.py`, `preprod_gate.py`. Determinism + replay gates wired in.
- Tested **AS, OFI, and PassiveMaker** strategies on Hyperliquid APE-perp.
- **AS-on-APE**: 0 fills under realistic queue + latency. Cancel-replace cadence (~3000/day) outpaces APE's trade arrival cadence (~275/day). Structurally wrong strategy class for this market.
- **OFI-on-ETH**: 1 fill across 24 hours. Signal too rare with current params on the available tape.
- **PassiveMaker** (built from scratch): captures real maker edge on some APE days (+$1.02 on 04-25), loses on others (-$1.38 on 05-05). Net **-$1.21 across 6 days** without gating.
- **Regime classifier** (vol × trend) implemented and integrated. Did not improve net PnL — single-axis features can't separate "real trend" from "intraday chop on a non-trending day" with 6-day data.
- **Real blocker: tape coverage.** 6 days of APE tape is too thin to validate any regime-conditional strategy. Bigger strategy redesigns or different instrument/venue are the only honest forward moves.

Test totals: **83 backtester unit + 173 strategy unit + 64 Python tests, all passing.**

---

## What was built (Phases 1–7)

Each phase ends with a working, tested module. Detailed implementation lives in commit history; this is the inventory.

### Phase 1 — Intra-day windowing
- `bpt-backtester/src/data/data_loader.cpp`: `parse_iso_ns()` accepts ISO 8601 with ns precision; `[start_ns, end_ns)` half-open filter at read time; sub-day windows now configurable.
- 5 new tests in `test_data_loader.cpp`. Existing tests updated to use realistic epoch-ns timestamps (small literals like `100` were below the new filter floor).

### Phase 2 — Determinism + multi-window unions
- `bpt-strategy/include/strategy/clock/sim_clock.h`: process-wide simulation clock with wall-clock fallback. Replaces 6 wall-clock reads in AS / funding_arb / short_vol that affected strategy output.
- `RNG audit`: zero RNG calls anywhere in the strategy code; 22 wall-clock reads classified.
- `[[simulation.windows]]` schema: multi-window unions for "all FOMC days", "every Asia open", etc. Sorted at config load. Integrated into `DataLoader`.
- Test target: `strategy_unit_tests` includes 5 SimClock tests.

### Phase 3 — Latency model + sessions
- `bpt-backtester/include/backtester/latency/latency_model.h`: abstract `LatencyModel` + `ParametricLatencyModel` (per-venue, per-leg base+jitter, seeded `mt19937_64`).
- `MatchingEngine` refactored to **defer match by `submit_to_match_ns`** (Option A from the latency-wiring discussion). `pending_submits_` queue, drain-before-event ordering. **Synchronous POST_ONLY-cross reject preserved** (real exchange contract).
- `pending_fills_` queue with `match_to_report_ns` delivery delay.
- `cancel_order` scans both `pending_submits_` and `pending_` (resting).
- `BacktesterApp` builds the model from `[simulation.latency]` config; legacy `cex_base_ms` / `hyperliquid_base_ms` translate forward with deprecation warning.
- `bpt-backtester/include/backtester/calendar/session_calendar.h`: crypto-relevant named windows (asian/european/us open/close). `[[simulation.sessions]]` config syntax expands at load.

### Phase 4 — Sweep + walk-forward + cell isolation
- `scripts/sweep.py` extended:
  - `--walk-forward "start=...,train=Nd,test=Nd,step=Nd,purge=Nm"`
  - `--parallel N` (cell envs allocated; concurrent execution gated on backtest.sh log isolation)
  - Per-cell unique metrics ports (fixed the CivetServer rebind bug that caused cells to fail back-to-back)
- `scripts/sweep_lib/walk_forward.py`: pure splitter. 8 tests.
- `scripts/sweep_lib/cell_env.py`: per-cell shm dir / metrics port / endpoint port allocator. 9 tests.

### Phase 5 — Queue regen on cancels-ahead
- `MatchingEngine::apply_queue_regen`: when book volume drops at our resting price more than the trade volume that hit there since last book update, attribute residual to cancels and decrement `queue_ahead` proportionally. Uniform-cancel approximation.
- `traded_since_book_` map, `price_key()` helper for double-keyed buckets.
- 5 tests covering trade-only, pure-cancel, mixed, level-disappears, and back-compat single-event.

### Phase 6 — Validation harness
- `scripts/validate.py` + `scripts/validate_lib/`: stdlib-only KS distance, `compare()` orchestrator, text + JSON report.
- Default thresholds: PnL 25%, fees 20%, fill count 10%, markout KS 10%, fill price KS 5%, equity curve KS 10%.
- 17 tests.

### Phase 7 — Pre-prod gate toolchain
- `scripts/sensitivity.py` + `sensitivity_lib/`: per-param ±perturb, elasticity = `max|Δpnl| / max(|baseline|, floor)`, fragile flag at threshold > 1.0.
- `scripts/preprod_gate.py` + `preprod_lib/`: 5-stage gate runner — baseline → walk_forward → sensitivity → determinism → replay. Each stage early-exits on fail; configurable thresholds; JSON output for CI.
- Determinism gate: byte-compares `trades.csv` + `summary.json` (modulo `wallclock_duration_ms`). Replay gate: tightened `validate.py` thresholds (5% PnL, 5% markout KS).

### Multi-process stack now buildable
- All 4 missing binaries built via Bazel: `bpt-strategy`, `bpt-md-gateway`, `bpt-order-gateway`, `bpt-refdata`. Copied into per-service `bin/` for `start.sh` to find.
- End-to-end smoke verified: 6 services come up, MD flows, orders place, fills can match.
- **Known stack bug**: `OkxOrderServer` rejects with "bad method" on WS upgrade — likely the bpt-order-gateway sends REST traffic instead of WS. Blocks running anything on OKX. Multi-hour debug task.

---

## Strategy research log

### AS on APE (the original target)
- Pre-existing strategy. 2026-05-07 baseline (pre-Phase-5): 582 fills, -1.58% return.
- Post-Phase-5 (queue regen now models APE's deep book correctly): **0 fills on 2026-05-08**.
- 4-cell hold-quote pilot (`requote_threshold ∈ {0.0005, 0.005} × min_half_spread ∈ {4, 12}`): all cells **0 fills**.
- **Verdict**: AS structurally incompatible with APE microstructure. APE has ~275 trades/day (1 every ~5 minutes) but AS reprices every ~30 seconds. Quotes never rest long enough at any one level to catch a trade.

### Depth survey across 10 HL instruments
On 2026-04-25 tape:
| Symbol | Mid | Spread (bps) | L1 USD depth |
|---|---|---|---|
| FARTCOIN | 0.20 | 2.5 | $476 |
| ZEC | 356 | 0.8 | $604 |
| **APE** | **0.17** | **8.2** | **$691** |
| TAO | 251 | 1.2 | $900 |
| AAVE | 95.8 | 1.8 | $960 |
| DOGE | 0.10 | 0.1 | $2,288 |
| HYPE | 41.6 | 0.2 | $2,303 |
| kPEPE | 0.004 | 2.6 | $3,309 |
| SOL | 86.6 | 0.1 | $35,019 |
| ETH | 2319 | 0.4 | $271,008 |

**Key finding**: APE has the widest spread on HL with reasonable book depth. Selecting a different HL instrument for AS doesn't help — every alternative either has spread too tight (<2bps) for AS to clear fees, or depth too deep ($35k+ L1) for $1k capital to fill.

### OFI on HL ETH 04-25
- Configured ETH/USD:PERPETUAL on default OFI params.
- Result: **1 fill across 24 hours.** OFI z-score crossed threshold once.
- 4-cell threshold sweep (entry_threshold ∈ {0.3, 1.0} × vol_gate ∈ {10, 30}): all cells **1 fill** — the same single signal event triggered all four.
- **Verdict**: Stack works for taker strategies (verified IOC matched, MAKER vs TAKER attribution correct). Signal too rare on ETH-04-25 with current params; needs more tape to assess.

### AS on OKX BTC-USDT (sanity check)
- Configured for the simulator-correctness check ("does AS earn somewhere").
- **Blocked by stack bug**: `OkxOrderServer` "accept error: bad method" — WS handshake fails because the gateway sends REST. Backtester crashes with `BacktestControlPublisher offer timed out`.
- **Inconclusive.** Can't test simulator soundness on OKX without the protocol fix.

### PassiveMakerStrategy (built from scratch)
**Design**: wide fixed half-spread, low cancel-replace cadence, inventory-skew via `r = FV - q × c`, three triggers (no resting orders, drift > requote_threshold, inventory cap).

Files added (~600 LOC + tests):
- `bpt-strategy/include/strategy/strategy/passive_maker_strategy.h`
- `bpt-strategy/src/strategy/passive_maker_strategy.cpp`
- `bpt-strategy/config/strategies/passive_maker.backtest.toml`
- `bpt-strategy/config/passive_maker.backtest.toml`
- `bpt-strategy/tests/unit/test_passive_maker.cpp`
- Registration in `StrategyFactory` + `BUILD`

**Multi-day result, baseline params** (`half_spread=25`, `inv_penalty=0.0001`):

| Date | Fills | PnL | Return | Win % |
|---|---|---|---|---|
| 04-25 | 33 | +$1.02 | **+0.10%** | 24.2 |
| 04-27 | 1 | +$0.02 | +0.002% | 0.0 |
| 05-05 | 114 | -$1.38 | -0.14% | 13.2 |
| 05-06 | 106 | -$0.59 | -0.06% | 14.2 |
| 05-07 | 118 | -$0.33 | -0.03% | 16.1 |
| 05-08 | 8 | +$0.05 | +0.005% | 25.0 |
| **Total** | **380** | **-$1.21** | **-0.12%** | — |

**Pattern**: high-fill days lose, medium-fill days win. The strategy over-quotes on volatile days; conditions for profitability and conditions for over-fill correlate.

### Regime classifier (vol × trend)
**Design**: 2-axis classifier from `RealizedVolEstimator` + new `trend_zscore = |Σ log returns| / (σ × √n)`. Three regimes — QUIET (low vol), TRENDING (high vol + high trend_z), CHOPPY (high vol + low trend_z) — with 4th case (single-tick spike) handled by existing `VolatilityGate`. Hysteresis to avoid flap.

Files added (~500 LOC + tests):
- `bpt-strategy/include/strategy/strategy/regime_classifier.h` (header-only utility)
- `bpt-strategy/tests/unit/test_regime_classifier.cpp` (7 tests)
- Integration into PassiveMakerStrategy

**Multi-day result comparison**:

| Date | No gate | Linear vol scale | Regime v1 (60s window) | Regime v2 (10min, z≥0.5) |
|---|---|---|---|---|
| 04-25 | 33 / **+$1.02** | 16 / +$0.06 | 1 / +$0.02 | 26 / -$0.34 |
| 04-27 | 1 / +$0.02 | 1 / +$0.02 | 1 / +$0.02 | 1 / +$0.02 |
| 05-05 | 114 / -$1.38 | 66 / **-$0.64** | 17 / -$0.73 | 108 / -$1.05 |
| 05-06 | 106 / -$0.59 | 69 / **-$0.31** | 89 / -$0.53 | 110 / -$0.59 |
| 05-07 | 118 / -$0.33 | 64 / -$0.29 | **24 / -$0.08** | 107 / -$0.34 |
| 05-08 | 8 / +$0.05 | 8 / +$0.05 | 8 / +$0.05 | 8 / +$0.05 |
| **Total** | **-$1.21** | **-$1.13** | -$1.27 | -$2.25 |

**Linear vol scaling** is the marginal best ($0.08 better over 6 days vs no gating), but kills the +$1.02 winning day in exchange for smaller losses on bad days.

**Regime v1** (60s window): pinned 04-25 as CHOPPY because minute-level retracements within a trending day kept dipping `trend_z` below threshold; 120s hysteresis latched onto CHOPPY.

**Regime v2** (10-min window, looser threshold): let in 04-25 to quote (good) but turned its profitable directional fills into a loss; barely gated 05-05/06/07 (bad).

**Verdict**: regime gating with single-axis (vol × trend_z) features doesn't separate the winning days from the losing days at this tape resolution. Each retune trades one error mode for another.

---

## Key empirical findings

1. **APE microstructure**: 8.2 bps median spread (widest on HL), $691 L1 depth, ~275 trades/day, ~4500 book updates/day. Daily price range 100-1300 bps. Tape availability: only 6 days locally (04-25, 04-27, 05-05, 05-06, 05-07, 05-08).

2. **APE regime structure**: Dominantly determined by `(realized_vol_per_minute, |trend_z|)` over the day:
   - High vol + high trend (04-25): profitable for PassiveMaker
   - High vol + low trend (05-05/06/07): unprofitable for PassiveMaker
   - Low vol (04-27, 05-08): break-even or small profit (few fills)

3. **Round-trip economics on APE for 25bps half-spread (50bps full)**:
   - Gross spread captured per RT: ~50 bps × $0.16 = $0.008
   - Round-trip maker fees (HL): 3 bps = $0.0005
   - Adverse markout per RT: typically -5 to -15 bps at 30s
   - Net edge per RT: ~$0.001 to $0.006
   - At qty_per_quote=50 APE (~$8 notional): tiny absolute PnL, scales linearly with size

4. **PassiveMaker fills on profitable days have negative markouts** but positive PnL. The strategy holds for minutes, longer than the markout horizons; round-trip captures real spread despite short-horizon adverse drift.

5. **OkxOrderServer protocol bug**: WS upgrade rejects with "bad method" — gateway sends REST, server expects WS. Blocks all OKX backtests.

---

## Blockers for further research

1. **Tape coverage** — the dominant blocker.
   - APE: 6 days. Way too few to validate regime classifiers, walk-forward, or even sized PnL distributions.
   - Other HL instruments: 1 day each (only 04-25).
   - OKX: 1 day BTC-USDT spot.
   - Need 30+ days of tape for any honest claim about strategy edge.

2. **OKX simulator path** — OkxOrderServer protocol mismatch. Multi-hour debug; blocks the simulator-sanity check ("can the simulator produce reasonable PnL for any strategy in any venue?").

3. **Strategy class fit** — both strategies tested on HL (AS, OFI) were calibrated for other markets (low-latency CEX, tighter-spread venues). PassiveMaker is the first strategy designed for HL's microstructure, and it's marginal.

---

## What's left to try (ordered by leverage)

1. **Get more tape**. 30 days of APE + 30 days of one other HL instrument. Without this everything else is speculation. Run `data-forge`'s `orderbook_fetcher` over a longer history.

2. **Funding-arb strategy on HL**. Multi-day signal that doesn't depend on intraday market making. Funding rates are 8h on most CEXs / hourly on HL. Capture the basis between perp and spot. Doesn't need fine-grained tape — daily snapshots could suffice for first-pass validation.

3. **Fix the OKX stack bug**. Unblocks AS-on-OKX simulator-sanity test. Likely 4-8 hours: trace the WS handshake mismatch, fix `OkxOrderServer` or the gateway adapter. Once fixed, OKX BTC-USDT is the cleanest "does the simulator work?" benchmark.

4. **Richer regime features**. Hurst exponent, multi-horizon autocorrelation of returns, signed order-flow imbalance trend, book-imbalance derivatives. Combine into a small ensemble. Each feature is a known signal in the literature; the value-add is testing them against each other on the same tape.

5. **Cross-venue arbitrage**. Once OKX path works AND there's tape for both venues on overlapping days, basis trades between OKX-perp and HL-perp on BTC become testable. This avoids the regime-classification problem entirely (the trade is mechanical).

---

## Files of note

### New (this session)
- `bpt-strategy/include/strategy/clock/sim_clock.h`
- `bpt-strategy/include/strategy/strategy/passive_maker_strategy.h` + `src/strategy/passive_maker_strategy.cpp`
- `bpt-strategy/include/strategy/strategy/regime_classifier.h` (header-only)
- `bpt-backtester/include/backtester/latency/latency_model.h` (header-only)
- `bpt-backtester/include/backtester/calendar/session_calendar.h` (header-only)
- `bpt-backtester/src/matching/matching_engine.cpp` — substantial refactor for Phase 3 + 5
- `bpt-strategy/config/passive_maker.backtest.toml` + `config/strategies/passive_maker.backtest.toml`
- `bpt-strategy/config/ofi.backtest.toml` + `config/strategies/ofi.backtest.toml`
- `bpt-refdata/config/bpt-refdata.backtest-okx.toml` + `config/exchanges/backtest-okx-mainnet.toml`
- `scripts/sweep.py` (refactored), `scripts/validate.py`, `scripts/sensitivity.py`, `scripts/preprod_gate.py`
- `scripts/sweep_lib/`, `scripts/validate_lib/`, `scripts/sensitivity_lib/`, `scripts/preprod_lib/`
- `scripts/tests/` (64 Python tests)
- This file: `RESEARCH_SUMMARY_2026_05.md`

### Modified
- `bpt-strategy/src/strategy/strategy_factory.cpp` — registered `PassiveMakerStrategy`
- `bpt-strategy/BUILD` — added passive_maker + tests
- `bpt-strategy/src/strategy/avellaneda_stoikov_strategy.cpp` — 4 sites switched to SimClock
- `bpt-strategy/src/strategy/funding_arb_strategy.cpp` — order_id seed via SimClock
- `bpt-strategy/src/strategy/short_vol_strategy.cpp` — portfolio ts via SimClock
- `bpt-strategy/src/app/strategy_app_backtest.cpp` — SimClock pinning in BacktestControl loop
- `bpt-backtester/src/data/data_loader.cpp` — full ISO 8601 ns parsing + window filter
- `bpt-backtester/src/matching/matching_engine.cpp` — deferred match (Phase 3) + queue regen (Phase 5)
- `bpt-backtester/src/config/loader.cpp` — multi-window, sessions, latency schemas
- `bpt-backtester/src/app/backtester_app.cpp` — LatencyModel construction
- `bpt-backtester/include/backtester/config/settings.h` — schema additions for windows, latency, sessions
- `bpt-backtester/config/bpt-backtester.hl-tape.toml` — repurposed across APE / ETH / different dates over the session

### Test inventory
- **Backtester unit (C++)**: 83 tests, all passing
- **Strategy unit (C++)**: 173 tests, all passing (was 151 pre-session)
- **Sweep + validate + sensitivity + preprod (Python)**: 64 tests, all passing

---

## How to resume

Quickest re-entry checklist for a future session:

1. Read this doc.
2. `cd /home/jseow/code/bpt-core` and run `bazel test //bpt-strategy:strategy_unit_tests` to confirm strategy tests still pass.
3. `cmake --build build --target backtester_unit_tests bpt-backtester -j` to verify backtester builds and tests pass.
4. `python -m pytest scripts/tests/` for the python toolchain.
5. To rerun the PassiveMaker multi-day baseline:
   ```bash
   cd /home/jseow/code/bpt-core
   # Set the date range in bpt-backtester/config/bpt-backtester.hl-tape.toml
   # Run: scripts/backtest.sh start bpt-strategy/config/passive_maker.backtest.toml
   ```
6. Tape lives in `/opt/bpt/data/backtest-cache/` (parquet, per-venue/per-symbol/per-day).
7. To restart strategy research with more tape: extend `data-forge/services/orderbook_fetcher` to download more APE history from OKX/HL, then re-run the multi-day loop.

The strategy research is at a clear decision point: get more tape, fix OKX, or pivot strategy class. The toolchain is ready for any of those.
