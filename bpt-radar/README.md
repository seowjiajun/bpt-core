# bpt-radar

Market-color aggregator. Consumes 6 internal streams (vol surface, MD BBO + trades,
instrument stats, funding rate, refdata perp metadata); produces one
`MarketColor` POD per refresh interval. Powers the dashboard `/radar` route.

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
flowchart TD
    pricer["pricer"]
    mdgw["md-gateway"]
    refdata["refdata"]
    bridge["bridge<br/>(→ console /radar)"]

    subgraph radar["bpt-radar"]
        surface_sub["surface_sub<br/>VolSurface"]
        stats_sub["stats_sub<br/>InstrumentStats<br/>(OI, mark, vol24h)"]
        funding_sub["funding_sub<br/>FundingRate"]
        refdata_perp["refdata_perp_sub<br/>perp id → underlying"]
        trade_sub["trade_sub<br/>MdTrade"]
        bbo_sub["bbo_sub<br/>MdMarketData"]

        analytics["<b>analytics</b><br/>GEX · max-pain · vol regime<br/>term spread · flow imbalance · basis"]

        color_pub["color_pub<br/>MarketColor (POD)"]

        surface_sub --> analytics
        stats_sub --> analytics
        funding_sub --> analytics
        refdata_perp --> analytics
        trade_sub --> analytics
        bbo_sub --> analytics
        analytics --> color_pub
    end

    pricer --> surface_sub
    mdgw --> stats_sub
    mdgw --> funding_sub
    mdgw --> trade_sub
    mdgw --> bbo_sub
    refdata --> refdata_perp
    color_pub --> bridge

    classDef external fill:#fff3cd,stroke:#856404,color:#000
    classDef domain fill:#dbeafe,stroke:#1e40af,stroke-width:2px,color:#000
    classDef layer fill:#f5f5f5,stroke:#333,color:#000
    class pricer,mdgw,refdata,bridge external
    class analytics domain
    class surface_sub,stats_sub,funding_sub,refdata_perp,trade_sub,bbo_sub,color_pub layer
```

## Streams produced

| Stream | ID | Contents | Cadence |
|---|---|---|---|
| `market_color` | 6002 | `MarketColor` (POD with options + perp + flow sections) | ~Hz per underlying |

## Streams consumed

| Stream | ID | Contents |
|---|---|---|
| `vol_surface` | 4001 | `VolSurface` |
| `instrument_stats` | 2004 | `InstrumentStats` (OI / mark / 24h vol) |
| `funding_rate` | 1005 | `FundingRate` |
| `refdata_snapshot` | 1001 | `RefDataSnapshot` (filtered to perps only — for instrument_id ↔ underlying map) |
| `md_data` | 2002 | `MdMarketData` + `MdTrade` (two separate subscribers, filter by templateId) |

## Layers (which this service has)

| Layer | Status | Notes |
|---|---|---|
| Composition root | yes | `src/main.cpp` |
| Service | yes | `app/radar_service.{h,cpp}` |
| Bus | yes | `messaging/aeron_bus.{h,cpp}` — `RadarBus` |
| Routing | **no** | — |
| Adapter | **no** | — |
| Wire | **no** | — |
| External codec | **no** | — |
| Pub/Sub (slow) | yes | 1 publisher + 6 subscribers, all api/aeron split |
| Pub (hot) | **no** | — |
| Internal codec | yes | `messaging/codecs/pod_market_color_codec.{h,cpp}` — POD memcpy |
| Domain logic | yes | `analysis/` — GEX calculation, max-pain solver, vol regime detection, flow imbalance window |

## Concepts used

- `bpt::common::codec::Codec<C, T>` — `PodMarketColorCodec` satisfies it.

## The two-subscriber-per-stream pattern

`md_data` (stream 2002) carries three SBE templates: `MdMarketData` (BBO),
`MdOrderBook` (L2), and `MdTrade`. Radar only needs BBO and trades — and
each goes to a different analyzer. So:

```
md_data stream ──→ MdMarketDataSubscriber (filters templateId, emits BBO)
                ──→ MdTradeSubscriber       (filters templateId, emits Trade)
```

Two subscribers on the same Aeron stream, each filtering by templateId.
Each pays only for the message family it consumes.

## Reading order

1. `src/main.cpp`
2. `app/radar_service.{h,cpp}` — wiring, fan-in, periodic publish.
3. `messaging/aeron_bus.{h,cpp}` — `RadarBus` shape (7 fields).
4. `analysis/gex.h`, `analysis/max_pain.h` — options-color analytics.
5. `messaging/market_color.h` — the output struct.

## Build + test

```bash
bazel build //bpt-radar:bpt-radar
bazel test //bpt-radar/...
```
