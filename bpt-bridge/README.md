# bpt-bridge

Aeron → WebSocket forwarder. Subscribes to 6 Aeron streams, translates SBE
to JSON, broadcasts over WebSocket to `bpt-console` (the React trading
console). The only service where the "external wire" is the operator's
browser instead of an exchange.

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
    strategy["strategy"]
    analytics["analytics"]
    radar["radar"]
    console["<b>bpt-console</b><br/>(React browser app)"]

    subgraph bridge["bpt-bridge"]
        md_sub["md_sub<br/>MdMarketData"]
        exec_sub["exec_sub<br/>ExecutionReport"]
        account_sub["account_sub<br/>AccountSnapshot"]
        portfolio_sub["portfolio_sub<br/>JSON blob"]
        tox_sub["tox_sub<br/>ToxicityUpdate"]
        color_sub["color_sub<br/>MarketColor"]

        encoder["<b>message_encoder</b><br/>SBE → JSON"]
        ws_srv["<b>WsServer</b><br/>(Boost.Beast)<br/>broadcasts + receives commands"]

        ctrl_pub["ctrl_pub<br/>console_control byte<br/>(HALT / RESUME)"]

        md_sub --> encoder
        exec_sub --> encoder
        account_sub --> encoder
        portfolio_sub --> encoder
        tox_sub --> encoder
        color_sub --> encoder
        encoder --> ws_srv
        ws_srv -->|"command<br/>(HALT/RESUME JSON)"| ctrl_pub
    end

    mdgw --> md_sub
    ogw --> exec_sub
    ogw --> account_sub
    strategy --> portfolio_sub
    analytics --> tox_sub
    radar --> color_sub
    ws_srv <-->|"WS / JSON<br/>port 8080"| console
    ctrl_pub --> strategy

    classDef external fill:#fff3cd,stroke:#856404,color:#000
    classDef domain fill:#dbeafe,stroke:#1e40af,stroke-width:2px,color:#000
    classDef layer fill:#f5f5f5,stroke:#333,color:#000
    class mdgw,ogw,strategy,analytics,radar,console external
    class encoder,ws_srv domain
    class md_sub,exec_sub,account_sub,portfolio_sub,tox_sub,color_sub,ctrl_pub layer
```

## Streams consumed (Aeron, inbound)

| Stream | ID | Contents |
|---|---|---|
| `md_data` | 2002 | `MdMarketData` (BBO) |
| `exec_report` | 3002 | `ExecutionReport` (decoded to `Fill` + `OrderEvent`) |
| `account_snapshot` | 3004 | `AccountSnapshot` (positions + balances) |
| `portfolio` | 9004 | JSON portfolio snapshots from strategy (multi-fragment, reassembled) |
| `toxicity` | 5001 | `ToxicityUpdate` |
| `market_color` | 6002 | `MarketColor` |

## Streams produced (Aeron, outbound)

| Stream | ID | Contents | Cadence |
|---|---|---|---|
| `console_control` | 9003 | 1-byte HALT (0x00) / RESUME (0x01) | on operator click |

## External wire

| Endpoint | Protocol | Direction |
|---|---|---|
| `ws://localhost:8080` | WebSocket / JSON | bridge → console: broadcasts |
| `ws://localhost:8080` | WebSocket / JSON commands | console → bridge: `{"kind":"halt"}` / `{"kind":"resume"}` |

JSON message kinds: `session`, `status`, `tick`, `fill`, `order`, `position`,
`toxicity`, `marketColor`. Schema in `bpt-console/frontend/src/types/messages.ts`.

## Layers (which this service has)

| Layer | Status | Notes |
|---|---|---|
| Composition root | yes | `src/main.cpp` |
| Service | yes | `app/bridge_service.{h,cpp}` — owns the event handlers |
| Bus | yes | `messaging/aeron_bus.{h,cpp}` — `BridgeBus` |
| Routing | **no** | one operator, one console |
| Adapter | **special** | the "adapter" equivalent is `WsServer` (outbound to console) — not an exchange |
| Wire | **yes** | `ws/ws_server.{h,cpp}` (Boost.Beast WebSocket server) |
| External codec | **yes** | `ws/message_encoder.{h,cpp}` — domain → JSON |
| Pub/Sub (slow) | yes | 1 publisher + 6 subscribers, all api/aeron split |
| Pub (hot) | **no** | — |
| Internal codec | **no** | all SBE decode is inline in the aeron subscribers |
| Domain logic | yes | `state/position_tracker.{h,cpp}` (running PnL), `aeron/sbe_decode.h` (decode helpers) |

## Special shape: WS server instead of WS client

Every other external-facing bpt-* service is a **client** to an external
WebSocket (the exchange). Bridge is the inverse — it **runs** a WebSocket
server that the console connects to.

That means:
- `ws/ws_server.h` plays the role md-gateway's `*MdWsClient` plays in
  reverse: it accepts inbound connections and broadcasts to all of them.
- `ws/message_encoder.h` plays the role of md-gateway's `*MdEncoder` —
  building outbound text (JSON instead of subscription URLs).
- There's no "decoder" sibling, but the WsServer does parse short text
  commands inbound (`{"kind":"halt"}`) — handled inline in `WsServer` and
  surfaced via `IBroadcaster::set_command_handler`.

## Test seam

`tests/unit/test_bridge_service_seam.cpp` — drives `BridgeService::on_*`
handlers directly with `FakeBroadcaster` + `FakeCtrlSink` (the latter
inherits `api::ConsoleControlPublisher`). No Aeron driver, no real
WebSocket listener. Verifies decode + broadcast for each input stream
and the HALT/RESUME command flow.

## Reading order

1. `src/main.cpp` — composition root, wires WsServer + BridgeService + AeronBus.
2. `app/bridge_service.{h,cpp}` — the `on_*` event handlers. The seam test drives these directly.
3. `messaging/aeron_bus.{h,cpp}` — `BridgeBus` shape (6 inbound subs + 1 outbound pub).
4. `ws/message_encoder.{h,cpp}` — SBE→JSON translation logic. Per-kind builders.
5. `ws/ws_server.{h,cpp}` — Boost.Beast WebSocket server, connection management.

## Build + test

```bash
bazel build //bpt-bridge:bpt-bridge
bazel test //bpt-bridge:bridge_seam_tests
```
