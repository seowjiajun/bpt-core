# bpt-refdata

Reference data service. Pulls instrument metadata (symbols, tick sizes, lot
sizes, expiries, strikes) + fee schedules from venue REST APIs; normalises
into a canonical `Instrument` model; publishes snapshots + deltas + fee
schedules over Aeron. Same external-facing shape as md-gateway but with REST
instead of WebSocket transport (cadence is once + periodic refresh, not
streaming).

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
    exchanges["<b>EXCHANGES (REST)</b><br/>Binance /exchangeInfo<br/>OKX /api/v5/public<br/>Deribit /get_instruments<br/>Hyperliquid /info"]
    consumers["<b>INTERNAL CONSUMERS</b><br/>strategy · pricer<br/>md-gateway · radar"]
    requester["any subscriber<br/>(RefDataSubscriptionRequest)"]

    subgraph refdata["bpt-refdata"]
        subgraph adapter["per-venue Adapter"]
            rest_client["RestClient<br/>(Boost.Beast over TLS)"]
            decoder["*RefdataDecoder<br/>(JSON → Instrument)"]
        end

        registry["<b>InstrumentRegistry</b><br/>(canonical IDs)"]
        resolver["<b>InstrumentResolver</b><br/>(venue ↔ canonical)"]

        snapshot["snapshot_sink<br/>RefDataSnapshot"]
        delta["delta_sink<br/>RefDataDelta + heartbeat"]
        fee["fee_sink<br/>FeeSchedule"]
        status["status_sink<br/>RefDataReady / Error"]
        ctrl["control_source"]

        decoder --> registry
        decoder --> resolver
        ctrl --> snapshot
        registry --> snapshot
        registry --> delta
        registry --> fee
        registry --> status
    end

    exchanges -->|"poll"| rest_client
    rest_client --> decoder
    requester --> ctrl
    snapshot --> consumers
    delta --> consumers
    fee --> consumers
    status --> consumers

    classDef external fill:#fff3cd,stroke:#856404,color:#000
    classDef domain fill:#dbeafe,stroke:#1e40af,stroke-width:2px,color:#000
    classDef layer fill:#f5f5f5,stroke:#333,color:#000
    class exchanges,consumers,requester external
    class registry,resolver domain
    class rest_client,decoder,snapshot,delta,fee,status,ctrl layer
```

## Streams produced

| Stream | ID | Contents | Cadence |
|---|---|---|---|
| `refdata_snapshot` | 1001 | `RefDataSnapshot` (full registry on subscriber request) | on-demand |
| `refdata_delta` | 1002 | `RefDataDelta` (per-instrument adds / status changes / heartbeat) | per-event + Hz heartbeat |
| `fee_schedule` | 1004 | `FeeSchedule` (per-instrument maker/taker bps) | one-shot at boot + on refresh |
| `refdata_status` | 1006 | `RefDataReady` (all enabled exchanges loaded), `RefDataError` | once at boot + per error |

## Streams consumed

| Stream | ID | Contents |
|---|---|---|
| `refdata_control` | 1003 | `RefDataSubscriptionRequest` from subscribers (asks for snapshot push) |

## Layers (which this service has)

| Layer | Status | Notes |
|---|---|---|
| Composition root | yes | `src/main.cpp` |
| Service | yes | `app/refdata_service.{h,cpp}` |
| Bus | yes | `messaging/aeron_bus.{h,cpp}` — `AeronBus` |
| Routing | yes | per-venue adapter handles its own venue's universe; `SubscriptionManager` (`messaging/subscription_manager.{h,cpp}`) routes `RefDataSubscriptionRequest` filters to the snapshot publisher |
| Adapter | yes | `adapter/<venue>/<venue>_refdata_adapter.{h,cpp}` — 4 venues |
| Wire | yes | `http/rest_client.{h,cpp}` (Boost.Beast over TLS, with retries — shared across venues) |
| External codec | yes | `adapter/<venue>/<venue>_refdata_decoder.{h,cpp}` (JSON → Instrument). OKX has an extra `okx_refdata_auth.{h,cpp}` for the OAuth dance |
| Pub/Sub (slow) | yes | 4 publishers + 1 subscriber, all api/aeron split |
| Pub (hot) | **no** | — |
| Internal codec | yes | `messaging/codecs/sbe_fee_schedule_codec.{h,cpp}`. RefDataSnapshot / RefDataDelta / RefDataReady are SBE-encoded inline in the publisher .cpp (they're snapshot-style, not high-cadence — no separate codec class) |
| Domain logic | yes | `registry/instrument_registry.{h,cpp}`, `resolver/instrument_resolver.{h,cpp}`, `mapping/` (canonical ID generation + venue ↔ canonical maps), `metrics/` |

## REST instead of WebSocket

Same adapter / wire / decoder shape as md-gateway, just:
- Wire is `rest_client.h` (one-shot HTTP GET with retries + TLS) instead of a streaming WS.
- Adapter polls on a schedule (initial fetch + periodic refresh) instead of running a forever-WS-receive loop.
- No subscription state — every poll fetches the full venue universe.

The `rest_client_lib` Bazel target is split out and re-used by `bpt-tape`
(which subclasses `RestClient` to tee response bodies to disk for the
refdata REST-capture feature).

## The control flow

`bpt-refdata` is unusual in that it has an inbound control stream
(`refdata_control`) — but the control isn't "subscribe to instruments"
(which md-gateway has). It's "send me a snapshot with this filter":

```cpp
struct RefdataRequest {
    uint64_t correlation_id;
    std::vector<InstrumentFilter> instruments;    // by symbol + exchange
    std::vector<CanonicalFilter>  canonical_filters;  // by base/quote/type
};
```

The service has the full universe locally; the request tells it which
slice to push on the snapshot stream.

## Test seams

- Unit: `tests/unit/` — registry, resolver, mapping, fee codec round-trip.
- Component: `tests/component/` — venue decoder fixtures (captured JSON → expected Instrument records).
- `tests/test_refdata_service_seam.cpp` — drives `RefdataService` event handlers directly with `FakeRefdataControlSource` / `FakeRefdataSnapshotSink` / etc. All Fakes inherit the corresponding `api::*` ports.

## Reading order

1. `src/main.cpp`
2. `app/refdata_service.{h,cpp}` — main loop, adapter fan-in, control-stream handling.
3. `messaging/aeron_bus.{h,cpp}` — `AeronBus` shape (4 pubs + 1 sub).
4. `adapter/binance/binance_refdata_adapter.{h,cpp}` — concrete venue example.
5. `registry/instrument_registry.{h,cpp}` — canonical universe + ID generation.
6. `resolver/instrument_resolver.{h,cpp}` — venue ↔ canonical lookup.

## Build + test

```bash
bazel build //bpt-refdata:bpt-refdata
bazel test //bpt-refdata:refdata_tests
```
