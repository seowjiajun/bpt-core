# bpt-backtester

Backtest harness. Two distinct binaries:

- **`bpt-backtester`** (multi-process) — runs as a normal Aeron service.
  Drives mock exchange WS servers + matching engine; replays tape into
  the live wire so the strategy runs unmodified. Acts as the integration
  smoke test for the wire.

- **`bpt-backtester-mono`** (single-process, deterministic) — strategy +
  matching engine + clock all in one thread, no Aeron. The measurement
  device for parameter tuning.

See [service-anatomy.md](../docs/service-anatomy.md) for the canonical service shape.

## At a glance — bpt-backtester (multi-process)

```mermaid
%%{init: {
  'theme': 'base',
  'themeVariables': {
    'fontFamily': '"SF Mono", "JetBrains Mono", "Cascadia Code", Consolas, monospace',
    'fontSize': '14px',
    'lineColor': '#475569',
    'primaryColor': '#1e293b',
    'primaryTextColor': '#f8fafc',
    'primaryBorderColor': '#0f172a'
  }
}}%%
flowchart TD
    tape["<b>TAPE FILES</b><br/>.wslog / .parquet"]
    services["<b>regular Aeron services</b><br/>strategy · md-gateway<br/>order-gateway"]

    subgraph backtester["bpt-backtester"]
        loader["DataLoader<br/>(reads tape)"]
        clock["<b>ClockMaster</b><br/>tick-by-tick replay driver"]

        subgraph mocks["mock venue servers"]
            md_srv["*MdServer<br/>(BinanceMdServer,<br/>OkxMdServer, ...)"]
            order_srv["*OrderServer"]
            info_srv["HyperliquidInfoServer<br/>(mock /info)"]
        end

        match_eng["<b>MatchingEngine</b><br/>(per-instrument book + queue)"]
        results["ResultsCollector"]

        ctrl_pub["ctrl_pub<br/>BacktestControl(seq, sim_ts)"]
        ack_sub["ack_sub<br/>BacktestAck(seq)"]

        loader --> clock
        clock -->|"MarketEvent"| mocks
        clock -->|"trade event"| match_eng
        match_eng -->|"fill"| results
        clock --> ctrl_pub
        ack_sub --> clock
    end

    tape --> loader
    mocks <-->|"WS / HTTP<br/>(localhost)"| services
    ctrl_pub --> services
    services --> ack_sub

    classDef external fill:#fff3cd,stroke:#856404,color:#000
    classDef domain fill:#dbeafe,stroke:#1e40af,stroke-width:2px,color:#000
    classDef layer fill:#f5f5f5,stroke:#333,color:#000
    class tape,services external
    class clock,match_eng domain
    class loader,md_srv,order_srv,info_srv,results,ctrl_pub,ack_sub layer
```

## At a glance — bpt-backtester-mono (deterministic)

```mermaid
%%{init: {
  'theme': 'base',
  'themeVariables': {
    'fontFamily': '"SF Mono", "JetBrains Mono", "Cascadia Code", Consolas, monospace',
    'fontSize': '14px',
    'lineColor': '#475569',
    'primaryColor': '#1e293b',
    'primaryTextColor': '#f8fafc',
    'primaryBorderColor': '#0f172a'
  }
}}%%
flowchart TD
    tape["<b>TAPE FILES</b><br/>.wslog"]

    subgraph mono["bpt-backtester-mono (single binary, single thread)"]
        harness["<b>StrategyHarness</b><br/>reads tape, drives one event at a time"]
        decoders["MdDecoders<br/>(reused from md-gateway)"]
        harness_pub["HarnessMdPublisher<br/>(in-process — no Aeron,<br/>no encode/decode)"]
        strategy["<b>IStrategy</b><br/>(same code as live)"]
        ogw_client["InProcessOrderGatewayClient"]
        match_eng["MatchingEngine<br/>(same as multi-process)"]
        results["fills · final equity log"]

        harness --> decoders
        decoders --> harness_pub
        harness_pub -->|"MdBbo / Trade / Book"| strategy
        strategy -->|"NewOrder"| ogw_client
        ogw_client --> match_eng
        match_eng -->|"fill"| strategy
        match_eng --> results
    end

    tape --> harness

    classDef external fill:#fff3cd,stroke:#856404,color:#000
    classDef domain fill:#dbeafe,stroke:#1e40af,stroke-width:2px,color:#000
    classDef layer fill:#f5f5f5,stroke:#333,color:#000
    class tape external
    class harness,strategy,match_eng domain
    class decoders,harness_pub,ogw_client,results layer
```

## Streams produced (multi-process)

| Stream | ID | Contents | Cadence |
|---|---|---|---|
| `backtest_control` | 9002 | `BacktestControl(seq, sim_ts)` — replay tick gate | per tick |

## Streams consumed (multi-process)

| Stream | ID | Contents |
|---|---|---|
| `backtest_ack` | 9001 | `BacktestAck(seq)` — strategy ack'd this tick |

## Layers (which this service has)

### bpt-backtester (multi-process)

| Layer | Status | Notes |
|---|---|---|
| Composition root | yes | `src/main.cpp` |
| Service | yes | `app/backtester_service.{h,cpp}` |
| Bus | yes | `messaging/aeron_bus.{h,cpp}` — `BacktesterBus` (ctrl_pub + ack_sub) |
| Routing | **no** | — |
| Adapter | **special** | mock venue servers, not real adapters. Per-venue: `BinanceMdServer`, `OkxOrderServer`, `HLInfoServer`, etc. |
| Wire | yes | the mock servers listen on local TCP — actual WS / HTTP servers using Boost.Beast |
| External codec | yes | mock servers emit venue JSON; strategy's adapters in md-gateway decode it normally |
| Pub/Sub (slow) | yes | 1 pub + 1 sub, api/aeron split |
| Pub (hot) | **no** | — |
| Internal codec | yes | `messaging/codecs/sbe_backtest_*.{h,cpp}` |
| Domain logic | yes | `data/` (DataLoader for tape replay), `clock/` (ClockMaster), `matching/` (MatchingEngine + book), `exchange/` (per-venue mock WS/HTTP servers), `results/` (collector + summary), `latency/` (configurable latency injector) |

### bpt-backtester-mono (single-process)

Drops most layers — the harness is a small library that wires strategy +
matching engine + tape replay end-to-end. See `harness/strategy_harness.h`
and `harness/inprocess_order_gateway_client.h`.

## The two binaries — why both

- The multi-process binary exercises the full wire (Aeron, SBE, WS clients,
  the whole stack). Catches integration bugs. Slow per-event because every
  message round-trips through shared memory + Java MediaDriver + decode.
- The mono binary is the measurement device. Same strategy code, same
  config, but no IPC means each tick is microseconds instead of
  milliseconds. Parameter sweeps that would take hours in multi-process
  finish in minutes here.

Tradeoff: mono can drift from multi-process behaviour over time if either
side changes the publish chain. We accept the drift and run both
periodically to verify they agree.

## Reading order

For multi-process:
1. `src/main.cpp`
2. `app/backtester_service.{h,cpp}` — wires DataLoader + ClockMaster + mocks.
3. `clock/clock_master.{h,cpp}` — the tick replay driver.
4. `data/data_loader.{h,cpp}` — reads tape, produces `MarketEvent` stream.
5. `matching/matching_engine.{h,cpp}` — per-instrument order book + fill logic.
6. `exchange/binance/binance_md_server.{h,cpp}` — example mock venue server.

For mono:
1. `src/main_mono.cpp`
2. `harness/strategy_harness.{h,cpp}` — the in-process driver.
3. `harness/inprocess_order_gateway_client.{h,cpp}` — strategy's order client without Aeron.

## Build

```bash
# multi-process — broken right now (HyperliquidMdDecoder signature drift)
bazel build //bpt-backtester:bpt-backtester

# mono — same blocker, plus kOrderBookDepth unqualified ref in test
bazel build //bpt-backtester:bpt-backtester-mono
bazel build //bpt-backtester:backtester_core  # core lib only — clean

# unit tests for core (config, matching, results)
bazel test //bpt-backtester:backtester_unit_tests
```

There are two pre-existing build issues (orthogonal to recent refactors):
- `strategy_harness.cpp`'s call to `HyperliquidMdDecoder::decode` is missing the
  `InstrumentStatsCallback` arg added in commit `8b62a7a`.
- `test_matching_engine.cpp:621` references `kOrderBookDepth` unqualified.

Both are fix-by-touching-2-lines but haven't been done yet. See
[`docs/backlog.md`](../docs/backlog.md) for current state.
