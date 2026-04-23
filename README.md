# bpt-core

Monorepo for a low-latency algorithmic trading system. All inter-service communication runs over [Aeron](https://github.com/real-logic/aeron) via the `transport/aeron` media driver.

## Services

| Service | Language | Role |
|---|---|---|
| **bpt-strategy** | C++ | Trading engine — strategy logic, order management, risk checks |
| **bpt-md-gateway** | C++ | Market data gateway (Binance, OKX, Hyperliquid, Deribit) |
| **bpt-order-gateway** | C++ | Order gateway — routes orders to exchanges, returns execution reports |
| **bpt-refdata** | C++ | Reference data service — instruments, fee schedules |
| **bpt-analytics** | C++ | Live markouts, toxicity scoring, fill rate, TTF |
| **bpt-pricer** | C++ | Implied volatility surface computation |
| **bpt-backtester** | C++ | Exchange simulator, reads Parquet market data |
| **dashboard/bridge** | C++ | Aeron → WebSocket forwarder for the dashboard frontend |
| **transport/aeron** | Java | Aeron media driver — central messaging backbone |
| **messages** | C++ | SBE message schemas and generated codecs (`bpt::messages`) |

## Requirements

- GCC 13+ (C++23)
- Bazel 7+ with bzlmod enabled
- Java 17 (for the Aeron media driver)
- Node.js 20 (for the dashboard frontend)

Bazel is the primary build system. CMake survives only for `bpt-backtester`
(Arrow/Parquet dep has no clean Bazel story yet) and will be retired entirely
once backtester's Parquet I/O is replaced or `rules_foreign_cc` is wired in.

## Building

### Bazel — everything except backtester

```bash
bazel build //...                       # all libs + binaries + tests
bazel test  //...                       # run all test targets
bazel build -c opt //...                # release build
bazel build //bpt-md-gateway/...        # one service
bazel build //bpt-order-gateway:bpt-order-gateway  # just a binary
```

Built artifacts land under `bazel-bin/<path>/` — symlinks into Bazel's
out-of-tree cache. `bazel-*` symlinks are `.gitignore`'d.

### CMake — backtester only

Backtester still requires CMake + vcpkg + Arrow/Parquet from the Apache apt
repo. Only set this up if you actually need to build `jormungandr`:

```bash
git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh -disableMetrics

cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --target jormungandr -j$(nproc)
ctest --test-dir build --output-on-failure -j$(nproc)
```

vcpkg packages consumed (backtester surface only):

```
fmt  tomlplusplus  boost-system  boost-json  prometheus-cpp  gtest
```

Plus `libarrow-dev` + `libparquet-dev` from the Apache apt repo.

## Running the stack (OKX demo)

```bash
./scripts/stack-testnet.sh start
./scripts/stack-testnet.sh status
./scripts/stack-testnet.sh stop
```

**Startup order:**
`transport/aeron` → `bpt-refdata` → `bpt-md-gateway` + `bpt-order-gateway` (parallel) → `bpt-strategy`

`bpt-strategy` blocks at startup until `bpt-refdata` publishes `RefDataReady` on stream 1006. If any configured exchange is missing from that signal, strategy halts.

## Deploying to a remote host

Clone the repo on the trading box, build with Bazel, install systemd units:

```bash
git clone git@github.com:bishanparktrading/bpt-core.git /opt/bpt/bpt-core
cd /opt/bpt/bpt-core
bazel build -c opt //...
./deploy/generate-units.sh
systemctl --user enable --now bpt-config-sync.timer
systemctl --user start bpt-stack.target
```

The `bpt-config-sync.timer` pulls config changes from origin daily; service
binaries get refreshed by re-running `bazel build -c opt //...` on the box
when code changes ship.

## Project layout

```
bpt-core/
  bpt-strategy/
    include/strategy/   # public headers
    src/                # implementation + main.cpp
    config/             # per-environment TOML configs
    tests/
  bpt-md-gateway/       # same layout
  bpt-order-gateway/
  bpt-refdata/
  bpt-analytics/
  bpt-pricer/
  bpt-backtester/
  dashboard/
    bridge/             # Aeron → WebSocket forwarder (C++)
    frontend/           # dashboard UI (not covered here)
  messages/             # SBE schemas + generated C++ codecs (bpt::messages)
  transport/
    aeron/              # Java media driver (Gradle)
  third_party/
    yggdrasil/          # shared C++ utility library (logging, Aeron utils, secrets, ...)
    *.BUILD             # Bazel build files for non-BCR http_archive deps
  deploy/
    generate-units.sh   # emit systemd user units for the stack
    env/                # per-environment env files
  scripts/
    stack.sh            # start/stop/status full stack
    stack-testnet.sh    # start/stop/status with OKX demo configs
    package.sh          # build release tarball
  MODULE.bazel          # Bazel module definition
  CMakeLists.txt        # CMake top-level (legacy path)
  .github/workflows/
    ci.yml              # build + test on push/PR
    release.yml         # package + publish on v* tags
```

## Aeron stream assignments

| Stream | Direction | Messages |
|---|---|---|
| 1001 | refdata → strategy | RefDataSnapshot |
| 1002 | refdata → strategy | RefDataDelta, Heartbeat |
| 1003 | strategy → refdata | RefDataSubscriptionRequest |
| 1004 | refdata → strategy | FeeSchedule |
| 1005 | md-gateway → strategy | FundingRate |
| 1006 | refdata → strategy | RefDataReady, RefDataError |
| 2001 | strategy → md-gateway | MdSubscribeBatch |
| 2002 | md-gateway → strategy | MdMarketData, MdTrade, MdOrderBook |
| 2003 | md-gateway → strategy | MdSubscriptionAck, Heartbeats |
| 3001 | strategy → order-gateway | NewOrder, CancelOrder, ModifyOrder, CancelAll |
| 3002 | order-gateway → strategy | ExecutionReport |
| 3003 | order-gateway → strategy | OrderGatewayHeartbeat |
| 4001 | pricer → strategy | VolSurface |
| 4002 | pricer → strategy | PricerHeartbeat, PricerReady |
| 5001 | analytics → strategy/dashboard | MarkoutReport, ToxicityScore, FillRateReport |
| 6001 | book → dashboard/strategy | BalanceSnapshot (consolidated multi-venue sub-account balances) |
| 9001 | strategy → backtester | BacktestAck (backtest mode only) |
| 9002 | backtester → strategy | BacktestControl (backtest mode only) |

## How new instruments reach prod

Two pipelines run on independent cadences:

```
Code changes (days–weeks)                   Config changes (daily)
──────────────────────                       ──────────────────────
Dev pushes C++ / Python change      bpt-ops instrument-mapping runs
         │                                      │ (GitHub Actions, 04:00 UTC)
         ▼                                      ▼
`v*` tag triggers release.yml       Opens PR to main with refreshed
         │                          config/instruments/*.json if changed
         ▼                                      │
Tarball uploaded as release asset               ▼
         │                          CI validates schema + refdata
         ▼                          fixture test still loads
Operator scps + `systemctl restart`             │
         │                                      ▼
         ▼                          Merged to main (auto-merge on green)
Running service picks up new binary             │
                                                ▼
                                    bpt-config-sync.timer on the trading
                                    box fires at 06:00 UTC daily
                                                │
                                    `sync-config.sh` does git pull --ff-only
                                                │
                                                ▼
                                    refdata's internal daily refresh tick
                                    re-reads config/instruments/ → publishes
                                    RefDataDelta on Aeron → strategies update
                                    their InstrumentCache.
```

Key property: mapping updates **never restart services**. Code changes **do**
restart the affected service. The two paths are deliberately orthogonal —
`git pull` on the trading box only makes sense because it's config-only from
the running-process perspective; the compiled binary lives outside the git
tree in the release tarball.

Set up on a trading box:

```bash
./deploy/generate-units.sh
systemctl --user enable --now bpt-config-sync.timer
systemctl --user start bpt-stack.target
systemctl --user list-timers bpt-config-sync.timer   # next scheduled run
journalctl --user -u bpt-config-sync -f              # tail the sync
```

## Configuration

Each service has a TOML config. The `[logging]` section is common to all:

```toml
[logging]
level             = "info"    # trace/debug/info/warn/error/critical/off
dir               = "logs"
flush_level       = "warn"    # force-flush on messages at this level or above
flush_interval_ms = 0         # also flush every N ms (0 = disable)
console           = true
file              = true
max_file_size_mb  = 10
max_files         = 3
```

## Secrets

Exchange API credentials are delivered to services via **systemd-creds**. Each service's unit declares:

```ini
LoadCredentialEncrypted=bpt-okx:/etc/bpt/creds/bpt-okx.cred
```

systemd decrypts the `.cred` at service start and mounts plaintext at `$CREDENTIALS_DIRECTORY/bpt-okx` on a per-service tmpfs. Encrypted files are bound to the TPM (or host key as fallback) and stored in the config repo. Secret files are parsed as `KEY=value` per line.

Encrypt a secret (one-time, at deploy):

```bash
sudo systemd-creds encrypt --name=bpt-okx - /etc/bpt/creds/bpt-okx.cred <<'EOF'
OKX_API_KEY=...
OKX_SECRET=...
OKX_PASSPHRASE=...
EOF
```

**Dev fallback:** when `$CREDENTIALS_DIRECTORY` is unset (running outside systemd), secrets are read from `~/.bpt-secrets/<name>` in the same `KEY=value` format.

## CI

GitHub Actions workflows are path-filtered so each PR only runs what's
actually needed:

| Workflow | Triggers on | What it does |
|---|---|---|
| `ci.yml` | C++ / transport / frontend changes | Bazel build+test, CMake backtester build, Gradle transport, npm frontend |
| `ci-ops.yml` | `bpt-ops/**` | ruff + pytest for the Python ops toolkit |
| `ci-mapping.yml` | `config/instruments/**` | diff guard on instrument mapping PRs |
| `ci-exchange-catalog.yml` | `messages/exchanges.yaml` + related | drift guard between YAML / Python / C++ catalogs |
| `ops-instrument-mapping.yml` | daily cron + manual | refreshes the instrument mapping, opens a PR |

Release is no longer tag-triggered — deploy is a `git pull` + `bazel build -c
opt //...` on the trading box, orchestrated by `deploy/generate-units.sh`.
