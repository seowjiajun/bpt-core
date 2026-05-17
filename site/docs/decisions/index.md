# Decisions

Opinionated choices, with reasoning. The point of this section isn't to claim
these are *the* right answers — it's to show that each was a deliberate pick over
a real alternative, with a tradeoff acknowledged.

| Decision | Choice | Main alternative considered |
|---|---|---|
| [IPC fabric](aeron.md) | Aeron over shared memory | nanomsg, ZeroMQ, Chronicle Queue, gRPC |
| [Service boundaries](hexagonal-bus.md) | Hexagonal — port interfaces at the bus seam | Direct Aeron coupling in app code |
| [Port layout](api-aeron-sim-layout.md) | `api/aeron/sim` folder + namespace split, no I-prefix | Flat layout with `I-` / `Aeron-` / `InProcess-` prefixes |
| [Hot-path dispatch](crtp-hot-path.md) | CRTP through `decoder → ValidatingPublisher<Inner> → Inner` | Virtual interfaces |
| [Orchestration](systemd-over-k8s.md) | systemd user units + tarball deploy | Kubernetes / Nomad |
| [Fill simulation](testnet-over-paper.md) | Real testnet capital; no synthetic fill engine | In-process paper-mode (removed after 45h trial) |

## Things deliberately not yet decided

- **Co-location vs cloud** for production trading hosts — testing on AWS Tokyo for now; bare-metal at LD4/NY4-equivalent only when latency budget demands it.
- **Strategy variant binary split** — one strategy per process today; all strategies in one binary later if redeploy cost becomes painful.
- **Risk service split** — risk currently lives in-process inside `bpt-order-gateway` (Option C). Promotion to a separate `bpt-risk` service (Option B) is gated on multi-gateway or multi-strategy with shared capital.
- **Aeron stream topology consolidation** — design captured + piloted; reverted because no stream-ID mismatch had actually bitten yet. Pick up when one does.

## Things explicitly rejected

- **Kafka anywhere on the trading path.** Kafka is for log-shaped data with seconds-to-minutes consumer lag tolerance, not microsecond IPC.
- **gRPC for service-to-service.** Adds protobuf parse + HTTP/2 framing overhead with no benefit over SBE-on-Aeron for in-host IPC.
- **Python on the hot path.** Used liberally for offline scripts (`scripts/*.py`), backtest converters, and tooling — never on a critical real-time path.
- **An ORM.** No relational store on the hot path; positions and PnL are in-memory + reconciled against the venue.
