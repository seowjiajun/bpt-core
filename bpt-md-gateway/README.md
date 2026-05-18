# bpt-md-gateway

Market-data gateway: subscribes to exchange WebSocket feeds (Binance / OKX /
Deribit / Hyperliquid), normalises into SBE messages, publishes on Aeron to
strategy + pricer + analytics + bridge + radar.

See [service-anatomy.md](../docs/service-anatomy.md) for the canonical
layered shape every bpt-* service follows. This README shows the specific
shape this service has.

## At a glance

![at-a-glance dataflow](diagrams/at-a-glance.svg)

> Source: [`diagrams/at-a-glance.d2`](diagrams/at-a-glance.d2). Re-render with `d2 --layout=elk diagrams/at-a-glance.d2 diagrams/at-a-glance.svg`.

**Legend**

| Edge colour | Path | What flows |
|---|---|---|
| 🟥 **Red** | Hot tick path | BBO / Trade / OrderBook (zero-copy SBE into Aeron log buffer) |
| 🟩 **Green** | Slow path | FundingRate / InstrumentStats / Ack (codec + offer) |
| 🟦 **Cyan** | Control | MdSubscribeBatch from strategy → SubscriptionManager → adapter / slow-pub |

## Detailed data flow (every major object)

Every component named, every edge labelled. Edge colour carries the path
semantic (see legend); rendered via D2 with ELK orthogonal routing.

![detailed dataflow](diagrams/detailed-flow.svg)

> Source: [`diagrams/detailed-flow.d2`](diagrams/detailed-flow.d2). Re-render with `d2 --layout=elk diagrams/detailed-flow.d2 diagrams/detailed-flow.svg`.

**Legend** (same colour scheme as the at-a-glance above)

| Edge colour | Path | Notes |
|---|---|---|
| 🟥 **Red** | Hot tick path (numbered 1–5) | Zero vtable, zero-copy at the Aeron boundary. ~µs. `MdPublisher` does validate + drop-rate breaker + SBE encode + offer in one step. |
| 🟩 **Green** | Slow path | Funding rate, instrument stats, ack. Same Aeron at the end, but goes through a `Codec` + `Publisher::offer` with a stack scratch buffer. ~µs per call, lower frequency. |
| 🟦 **Cyan** | Control / ack (lettered a–g) | `MdSubscribeBatch` from strategy → `MdControlSubscriber` → `MdGatewayService` → `SubscriptionManager` → ack via SBE + adapter subscribe. |
| ⬜ **Slate** | Composition / lifetime | One-shot ownership wiring — main builds factory, factory constructs bus, service owns components. |

**The control path (lettered a–g):** strategy publishes `MdSubscribeBatch` → flows through Aeron → `MdControlSubscriber` decodes → `MdGatewayService` routes via `SubscriptionManager` → which both (d) publishes an ack and (e) tells the venue adapter to subscribe → which builds the URL and hands to the WS client → which reconnects with the new subscriptions baked into the URL (Binance) or sends a runtime subscribe frame (OKX / Deribit / HL).

## Streams produced

| Stream | ID | Contents | Cadence |
|---|---|---|---|
| `md_data` | 2002 | MdBbo, MdTrade, MdOrderBook (SBE) | kHz per active instrument |
| `md_ack_hb` | 2003 | MdSubscriptionAck, MdSubscriptionHeartbeat, MdServiceHeartbeat | Hz |
| `funding_rate` | 1005 | FundingRate updates | ~Hz per perp instrument |
| `instrument_stats` | 2004 | InstrumentStats (OI, mark, index, last, 24h vol) | per-instrument ~10s |

## Streams consumed

| Stream | ID | Contents |
|---|---|---|
| `md_control` | 2001 | MdSubscribeBatch from strategy (the only inbound) |

## Layers (canonical shape)

| Layer | Code location | Files |
|---|---|---|
| Composition root | `src/main.cpp` | — |
| Service | `app/md_gateway_service.{h,cpp}` | `MdGatewayService` (IService impl) |
| Bus | `messaging/aeron_bus.{h,cpp}` | `MdGatewayBus` struct + `build()` factory |
| Routing | `subscription/subscription_manager.{h,cpp}` | `SubscriptionManager` |
| Adapter | `adapter/<venue>/<venue>_md_adapter.{h,cpp}` | 4 adapters, share `adapter/common/adapter_base.h` |
| Wire | `adapter/<venue>/<venue>_md_ws_client.{h,cpp}` | Boost.Beast + Asio WS |
| External codec | `adapter/<venue>/<venue>_md_decoder.h` (header-only template), `<venue>_md_encoder.{h,cpp}` | simdjson decoder, free-function encoder |
| Pub/Sub (slow) | `messaging/publishers/{api,aeron}/...`, `messaging/subscribers/{api,aeron}/...` | api/aeron split |
| Pub (hot) | `messaging/publishers/md_publisher.{h,cpp}` | `MdPublisher` (templated chain target) |
| Internal codec | `messaging/codecs/sbe_*.{h,cpp}` | All satisfy `Codec<C, T>` |
| Hot-path support | `md/{md_encoder,md_validator,md_types,md_publisher_concept,validation_drop_breaker,...}.h` | template chain + MdPublisher-owned validator/breaker pieces |

## Concepts

| Concept | Where defined | Used by |
|---|---|---|
| `MdSink<P>` | `md/md_publisher_concept.h` | All 4 venue decoders constrain their `Pub` template param |
| `MdPublisher<P>` | `md/md_publisher_concept.h` | Prod `MdPublisher` self-verifies via `static_assert` |
| `Codec<C, T>` | `bpt-common/include/bpt_common/codec/codec.h` | All slow-path SBE codecs verify via `static_assert` |

## Test seams

- **Component tests**: `tests/component/test_<venue>_adapter.cpp` — captured JSON fragments → expected SBE output. Uses `FakeMdPublisher` (satisfies `MdSink` concept without inheriting any port).
- **Unit tests**: `tests/unit/test_*.cpp` — codec round-trips, validator, drop-breaker, subscription manager.
- **Component test fake for AckPublisher**: `tests/component/fake_ack_publisher.h` — inherits `api::AckPublisher` port.

## Hot path vs slow path summary

| | Hot path | Slow path |
|---|---|---|
| What | MD ticks (BBO/Trade/OrderBook) | Funding rate, instrument stats, acks, heartbeats |
| Rate | kHz per instrument | Hz to 0.01 Hz |
| Dispatch | Template composition + concept (`MdSink`) | Virtual port (api/aeron split) |
| Encode | Zero-copy SBE via `MdPublisher::tryClaim` | `Codec<C,T>::encode(obj, scratch)` then `offer` |
| Vtable hops | 0 | 1 per publish |
| Files | `md/*.h`, `messaging/publishers/md_publisher.h` | `messaging/publishers/{api,aeron}/...` |

## Reading order for new contributors

1. **`src/main.cpp`** — what gets wired up (composition root).
2. **`app/md_gateway_service.{h,cpp}`** — the poll loop. See how `bus_` is consumed.
3. **`messaging/aeron_bus.{h,cpp}`** — what the bus owns (one struct field per stream this service produces/consumes).
4. **`adapter/common/i_adapter.h`** — adapter contract. The file's `@file` doc has an ASCII picture of the per-venue stack.
5. **`adapter/binance/binance_md_adapter.h`** — concrete venue adapter. Other 3 venues follow the same shape.
6. **`adapter/binance/binance_md_decoder.h`** — concept-constrained template doing JSON→domain.
7. **`messaging/publishers/md_publisher.h`** — validate → drop-rate breaker → zero-copy SBE encode + Aeron `tryClaim`. The hot path lives entirely here.

Everything else (subscription manager, individual SBE codecs, per-venue exec
decoders) follows from those seven files.

## Build + test

```bash
bazel build //bpt-md-gateway:bpt-md-gateway
bazel test //bpt-md-gateway/...      # unit + component tests
```

Hot-path latency target: BBO JSON-frame to MD-stream offer in <10 µs p50 on
warm-cache. Measured via `BinanceMdDecoder::decode_lat_` histogram; sampled
to Prometheus every 5 s.
