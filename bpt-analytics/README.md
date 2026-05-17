# bpt-analytics

Live trading analytics — markouts, toxicity scoring, fill-rate, time-to-fill.
Consumes MD BBO + exec reports; emits `ToxicityUpdate` POD messages. Pure
internal-consumer service.

See [service-anatomy.md](../docs/service-anatomy.md) for the canonical service shape.

## At a glance

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
flowchart LR
    mdgw["md-gateway"]
    ogw["order-gateway"]
    strategy["strategy · bridge"]

    subgraph analytics["bpt-analytics"]
        md_sub["md_sub<br/>MdBbo"]
        exec_sub["exec_sub<br/>ExecutionReport"]
        analyzers["<b>analyzers</b><br/>ToxicityCalculator<br/>MarkoutAnalyzer<br/>FillRateTracker"]
        tox_pub["tox_pub<br/>ToxicityUpdate (POD)"]

        md_sub --> analyzers
        exec_sub --> analyzers
        analyzers --> tox_pub
    end

    mdgw --> md_sub
    ogw --> exec_sub
    tox_pub --> strategy

    classDef external fill:#fff3cd,stroke:#856404,color:#000
    classDef domain fill:#dbeafe,stroke:#1e40af,stroke-width:2px,color:#000
    classDef layer fill:#f5f5f5,stroke:#333,color:#000
    class mdgw,ogw,strategy external
    class analyzers domain
    class md_sub,exec_sub,tox_pub layer
```

## Streams produced

| Stream | ID | Contents | Cadence |
|---|---|---|---|
| `toxicity` | 5001 | `ToxicityUpdate` (POD, not SBE — fixed-layout struct) | ~Hz per active instrument |

## Streams consumed

| Stream | ID | Contents |
|---|---|---|
| `md_data` | 2002 | `MdBbo` |
| `exec_report` | 3002 | `ExecutionReport` |

## Layers (which this service has)

| Layer | Status | Notes |
|---|---|---|
| Composition root | yes | `src/main.cpp` |
| Service | yes | `app/analytics_service.{h,cpp}` |
| Bus | yes | `messaging/aeron_bus.{h,cpp}` — `AnalyticsBus` |
| Routing | **no** | — |
| Adapter | **no** | — |
| Wire | **no** | — |
| External codec | **no** | — |
| Pub/Sub (slow) | yes | `publishers/{api,aeron}/toxicity_publisher.h`, `subscribers/{api,aeron}/...` |
| Pub (hot) | **no** | — |
| Internal codec | yes | `messaging/codecs/pod_toxicity_codec.{h,cpp}` (POD memcpy codec — fixed-layout struct, not SBE) |
| Domain logic | yes | `analysis/` — `ToxicityCalculator`, `MarkoutAnalyzer`, `FillRateTracker`, `TimeToFillTracker` |

## Concepts used

- `bpt::common::codec::Codec<C, T>` — `PodToxicityCodec` satisfies it.

## Test seams

- Unit: `tests/unit/` — per-analyzer (markout, fill-rate, etc.)
- No component tests (no external venue).

## POD codec vs SBE codec

Most internal codecs are SBE-encoded (versioned, variable-length). Toxicity
is a small fixed-layout POD with no nested types or strings, so the codec
is a memcpy round-trip — same `Codec<C, T>` contract, different
implementation. See `PodToxicityCodec`.

## Reading order

1. `src/main.cpp`
2. `app/analytics_service.{h,cpp}` — main poll loop, wires subs to analyzers, drives publish cadence.
3. `messaging/aeron_bus.{h,cpp}` — `AnalyticsBus` shape.
4. `analysis/toxicity_calculator.h` — the headline analytic.

## Build + test

```bash
bazel build //bpt-analytics:bpt-analytics
bazel test //bpt-analytics/...
```
