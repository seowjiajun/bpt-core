# bpt-core

> Low-latency algorithmic trading system spanning **C++ • Java • Python • TypeScript**, built around the Aeron messaging fabric. Eight backend services + a real-time React/WebSocket dashboard, end-to-end live on Hyperliquid testnet/mainnet.

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

---

A solo project. Built end-to-end — services, deploy tooling, dashboard, and a backtest harness — to learn what a production trading stack actually feels like to operate. Live on Hyperliquid testnet; mainnet read-only verified on PURR + HYPE funding-arb and XMR market making.

<p align="center">
  <img src="docs/diagrams/system-overview.svg" alt="bpt-core system overview" width="900">
</p>

Deeper diagrams (tick→fill sequence, config topology) live in [`docs/architecture.md`](docs/architecture.md).

## What's here

| Service | Language | Role |
|---|---|---|
| `bpt-strategy` | C++23 | Strategy framework — startup gates, sizing, dashboard publishing, per-strategy panels |
| `bpt-md-gateway` | C++23 | Market data gateway — Binance / OKX / Hyperliquid / Deribit WebSocket adapters with validation breaker |
| `bpt-order-gateway` | C++23 | Order routing — multi-venue execution, position tracking, risk + reject-rate circuit breakers |
| `bpt-refdata` | C++23 | Reference data — instrument catalog, fee schedules, per-venue REST adapters, canonical ID mapping |
| `bpt-analytics` | C++23 | Live toxicity scoring, markouts, fill-rate analytics |
| `bpt-pricer` | C++23 | Black-Scholes implied volatility surface computation |
| `bpt-pms` | C++23 | Multi-venue balance + position aggregator |
| `bpt-tape` | C++23 | Market-data recorder (Parquet → S3) for backtest replay |
| `bpt-backtester` | C++23 | Exchange simulator — replays orderbook tape against the strategy + order-gateway path |
| `bpt-bridge` | C++23 | Aeron → WebSocket forwarder for the live dashboard |
| `dashboard/frontend` | React + TypeScript | Real-time trading dashboard with per-strategy panel + chart registries |
| `messages` | SBE | Zero-copy / zero-allocation wire schemas (v9+) |
| `transport/aeron` | Java 17 | External Aeron MediaDriver (ZGC-tuned to isolate GC from the C++ hot path) |

## Architecture highlights

**Messaging.** Every inter-service hop runs over Aeron with SBE-encoded payloads. No service connects to another directly — all coordination is publish/subscribe over a shared MediaDriver. The MediaDriver is an external Java process (rather than embedded) so its GC can never stall the C++ services.

**Single source of truth for stream IDs.** `deploy/config/aeron/streams.toml` owns every Aeron stream ID and the MediaDriver path. Each service's TOML references it via `aeron_config = "..."` and resolves its streams by global name — typo fails at boot rather than silently subscribing to the wrong topic.

**Single source of truth for deployment.** `deploy/config/profile/<tag>.toml` owns environment, exchange filter, and exchange-endpoints path. `switch-env.sh` refuses to activate any env file whose services disagree on profile. Eliminates the "stack started on different exchanges" failure mode.

**Per-strategy modularity.** The dashboard has a `STRATEGY_PANELS` registry (React) and a `STRATEGY_CHARTS` registry — adding a new strategy means writing a panel/chart component and registering it. The bridge stays a dumb pipe; per-strategy concerns live in per-strategy code. See `dashboard/frontend/src/components/panels/` and `components/charts/`.

**Zero-copy hot path.** SBE encoders/decoders generate header-only classes; no heap allocation on tick processing. Strategy → order-gateway round-trip measured at ~150 µs p50 / 524 µs max on commodity hardware.

**Safety guards.**
- Environment / exchange-config mismatch (e.g. `env=prod` with testnet endpoints) fails boot.
- `switch-env.sh` coherence-checks every picked config before flipping the symlink.
- Refdata health is gated — strategy refuses to trade if `RefDataReady.exchangesLoaded` is missing any configured venue.
- Min-notional checks before order submit (e.g. HL's $10 floor is bumped automatically in qty).

## Live screenshots

See `docs/screenshots/` for the dashboard running on live Hyperliquid data.
- AvellanedaStoikov on XMR perp: candlestick + bid/ask overlays + AS-specific state panel.
- FundingArb on PURR/HYPE: dual-leg spot/perp chart, basis bps overlay, funding APR.
- PassiveMaker on APE: lifecycle including order rest → fill → realized P&L.

## Try it

Prerequisites:
- GCC 13+ (C++23)
- Bazel 7+ with bzlmod
- Java 17 (Aeron MediaDriver)
- Node 20 (dashboard)
- Linux (systemd-user for the deploy scripts; macOS works for dev with manual launch)

```bash
git clone https://github.com/seowjiajun/bpt-core.git
cd bpt-core
bazel build //...                                # ~5 min cold
bazel test //...                                 # ~30 s
cd transport/aeron && ./gradlew shadowJar        # builds the MediaDriver
cd ../../dashboard/frontend && npm ci && npm run build
```

Bring up the stack on Hyperliquid testnet (no real-money risk; orders fire against HL's testnet venue):

```bash
./deploy/generate-units-dev.sh                   # writes systemd-user units
cp deploy/env/dev.env.example deploy/env/dev-hyperliquid.env
# edit the env file — wallet address, secret name (testnet wallets are free from HL's faucet)
./deploy/switch-env.sh dev-hyperliquid           # symlinks active.env + restarts stack
journalctl --user -fu bpt-strategy               # tail the strategy log
```

Open `http://localhost:5173/` for the live dashboard.

## Repo layout

```
bpt-<service>/                  one Bazel-built C++ service
  include/<service>/            public headers
  src/<service>/                impl
  config/                       per-stack TOMLs
  tests/unit/                   googletest
  BUILD                         bazel
bpt-bridge/                     C++ Aeron→WS forwarder (sibling of other bpt-* services)
dashboard/
  frontend/                     Vite + React + TypeScript
  scripts/                      run/smoke helpers
deploy/
  config/aeron/streams.toml     shared stream registry
  config/profile/*.toml         deployment profiles
  env/*.env.example             stack templates
  generate-units*.sh            systemd unit generators
  switch-env.sh                 coherence-checked env switcher
messages/
  schema/bpt-protocol.xml       SBE schemas
  generated/cpp/                generated codecs (checked-in)
infra/terraform/                AWS for the monitoring + tape hosts
```

## Strategies included

- **PassiveMakerStrategy** — three-knob symmetric market maker. Useful as a worked example of how to extend the framework.
- **AvellanedaStoikovStrategy** — full Stoikov '08 with drift / regime / queue suppression layers and inventory-adaptive sizing.
- **FundingArbStrategy** — paired spot + perp position to capture funding-rate carry, delta-neutral.
- **OFI / Momentum / VwapReversion / RegimeSwitch / HMM / ShortVol** — additional well-known algorithms.

The interesting bits are less the strategies themselves (mostly textbook) and more the supporting machinery — `RegimeDetector`, `OFICalculator`, `HmmFilter`, `QueueTracker`, `RealizedVolEstimator` — and how they thread through the `IStrategy` interface so that adding a new strategy is one file plus a factory registration. Parameter values in the configs are calibration starting points, not capacity-tested production tunings; use the `bpt-backtester` harness to re-fit them for your own data.

## Things I'd build next given more time

- **Cross-exchange perp funding arb** (HL ↔ Binance ↔ OKX). Same-asset funding differential, market-neutral. The existing `FundingArbStrategy` generalises to N venues with the adapters already in place.
- **Lock-free order book in C++** for the order-gateway adapters. Currently the matching-side state uses standard containers; a custom flat array + atomic price-level updates would cut p99 by ~30%.
- **GitHub Actions CI** with Bazel build + test + clang-tidy + ruff + prettier.
- **Onchain MEV scanner** as a sibling service (Solidity / Foundry) — DEX arbitrage opportunities at L1 finality. Different latency profile, useful contrast to CEX flow.

## Operational notes

- Deployed to a single AWS host (Singapore, c6gn class). Monitoring host runs Prometheus + Grafana + a Healthchecks.io heartbeat for the tape service. Terraform in `infra/`.
- Bazel build cache hits make local dev fast after the first build. Cold build ~5 min, warm ~10 s.

---

Built by [Jia Jun Seow](https://github.com/seowjiajun). Questions / interview interest: open an issue or DM.
