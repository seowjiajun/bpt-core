# Architecture

Diagrams here complement the high-level overview in the [README](../README.md). Sources are D2 (`docs/diagrams/*.d2`) — re-render with `d2 docs/diagrams/foo.d2 docs/diagrams/foo.svg` after edits.

For the shape of an individual service (composition root → service → bus → adapter → wire / codec → pub/sub), see [service-anatomy.md](service-anatomy.md). That doc covers the canonical layered structure every bpt-* C++ service follows and which layers each service drops.

## Context

The highest-altitude view — bpt-core sitting in its operating environment.
Who interacts with it, what it depends on, what it produces.

```mermaid
C4Context
    title bpt-core in context

    Person(operator, "Operator", "Solo trader / sysadmin.<br/>Halt/resume, monitors PnL,<br/>tunes strategy params.")

    System_Boundary(bpt, "bpt-core trading stack") {
        System(stack, "bpt-core", "Eight C++ services + React console.<br/>Aeron+SBE inter-service IPC.<br/>One binary per service.")
    }

    System_Ext(binance, "Binance", "Spot + futures<br/>WebSocket + REST")
    System_Ext(okx, "OKX", "Spot + perps<br/>WebSocket + REST")
    System_Ext(deribit, "Deribit", "Options<br/>WebSocket + REST")
    System_Ext(hl, "Hyperliquid", "Perps + spot<br/>WebSocket + REST")

    System_Ext(aws, "AWS Tokyo", "Prod host (EC2)<br/>+ S3 tape archive<br/>+ Grafana monitoring host")

    Rel(operator, stack, "Operates", "WebSocket console<br/>(localhost:5173)")
    Rel(stack, binance, "Market data + orders", "WS / HTTPS / TLS")
    Rel(stack, okx, "Market data + orders", "WS / HTTPS / TLS")
    Rel(stack, deribit, "Market data + orders", "WS / HTTPS / TLS")
    Rel(stack, hl, "Market data + orders", "WS / HTTPS / TLS")
    Rel(stack, aws, "Deploys + archives tape", "systemd / rclone")

    UpdateLayoutConfig($c4ShapeInRow="3", $c4BoundaryInRow="1")
```

## System overview

The whole stack at a glance: exchange venues on one side, the React console on the other, every C++ service multiplexed over an Aeron fabric in the middle.

<p align="center">
  <img src="diagrams/system-overview.svg" alt="bpt-core system overview" width="900">
</p>

The Aeron MediaDriver runs as an external JVM process (rather than embedded in any C++ service) so its garbage collection can never stall the hot path. Every inter-service message is SBE-encoded — zero-copy on the C++ side.

## Tick → order → fill

A single market-data update through to fill, showing the message hops and the work each service does. The strategy → order-gateway round-trip is measured at ~150µs p50 / 524µs max on commodity hardware.

<p align="center">
  <img src="diagrams/tick-to-fill.svg" alt="tick to fill sequence" width="900">
</p>

The path is intentionally short: no broker, no internal queueing layer, no thrift / gRPC hop. Strategies subscribe to the same SBE-decoded view of the market that the gateway publishes, and ship `OrderRequest` straight back over the same fabric. The order-gateway owns risk + circuit-breakers — there's no separate risk service in the hot path.

### Sequence view

The same flow as a sequence diagram — showing the actual call sites and what runs in which process. Useful for measuring where the budget goes between the wire arrival and the fill landing.

```mermaid
%%{init: {
  'theme': 'base',
  'themeVariables': {
    'fontFamily': '"SF Mono", "JetBrains Mono", Consolas, monospace',
    'fontSize': '13px',
    'primaryColor': '#1e293b',
    'primaryTextColor': '#f8fafc',
    'actorBkg': '#1e293b',
    'actorBorder': '#0f172a',
    'actorTextColor': '#f8fafc',
    'noteBkgColor': '#fef3c7',
    'noteBorderColor': '#b45309',
    'noteTextColor': '#451a03',
    'sequenceNumberColor': '#fafafa',
    'signalColor': '#475569',
    'signalTextColor': '#0f172a'
  }
}}%%
sequenceDiagram
    autonumber
    participant venue as 📡 Exchange WS
    participant mdgw as bpt-md-gateway
    participant aeron as ☕ Aeron MediaDriver
    participant strat as bpt-strategy
    participant ogw as bpt-order-gateway
    participant venue2 as 📡 Exchange REST

    venue->>mdgw: BBO tick (JSON frame)
    Note over mdgw: simdjson parse →<br/>MdBbo POD
    mdgw->>mdgw: ValidatingPublisher checks
    mdgw->>aeron: SBE encode + tryClaim
    Note over aeron: zero-copy into log buffer
    aeron->>strat: BBO delivered
    Note over strat: strategy logic<br/>(AS / OptionsMaker / ...)
    strat->>aeron: NewOrder SBE
    aeron->>ogw: NewOrder delivered
    Note over ogw: risk gates<br/>(pre-trade, kill switch,<br/>min-notional)
    ogw->>venue2: signed POST /api/order
    venue2-->>ogw: ack (orderId)
    Note over ogw: ExecutionReport ACKED
    ogw->>aeron: ExecutionReport SBE
    aeron->>strat: exec delivered
    venue2-->>ogw: fill (WS user-data)
    Note over ogw: ExecutionReport FILLED
    ogw->>aeron: ExecutionReport SBE
    aeron->>strat: fill delivered
```

**Budget breakdown** (~150 µs p50 wire-arrival to fill-on-strategy):
- ~10 µs: WS frame → simdjson parse → MdBbo
- ~5 µs: ValidatingPublisher + SBE encode + Aeron offer
- ~20 µs: Aeron shared-memory delivery to strategy
- ~30 µs: strategy logic + NewOrder publish
- ~50 µs: order-gateway risk + REST POST
- ~30 µs: ack round-trip back to strategy

## Configuration topology

The deploy story is built around three "single sources of truth" plus one coherence-checking gate. Misconfiguration is caught at boot (or earlier, when `switch-env.sh` lints the env file) rather than mid-trading.

<p align="center">
  <img src="diagrams/config-topology.svg" alt="configuration topology" width="900">
</p>

- **`deploy/config/aeron/streams.toml`** owns every Aeron stream ID. Services reference streams by global name; a typo fails at boot instead of silently subscribing to the wrong topic.
- **`deploy/config/profile/<tag>.toml`** owns environment + exchange filter + endpoints path. Each service config references it as `profile_config = "..."`.
- **`deploy/env/<stack>.env`** is the per-stack environment file; `switch-env.sh` symlinks the active one and refuses to activate any env whose services disagree on profile.
- **systemd user units** read the env file at start; they don't bake any config into the unit itself.

This eliminates the "stack started on different exchanges" and "two services subscribed to mismatched stream IDs" classes of failure that bite multi-service systems early on.
