# Operations

What it takes to actually run the stack — not just build it. Operations is
the thing that separates "I wrote some code" from "I run a system."

## Pages here

- [Monitoring](monitoring.md) — Prometheus + Alertmanager + Grafana + PagerDuty + Healthchecks.io
- [Risk](risk.md) — six pre-trade gates + three post-trade latches in `bpt-order-gateway`
- [Deployment](deployment.md) — tarball + atomic symlink flip; rollback; first-prod-host validation
- [Host sleep / WSL suspend](host-sleep-recovery.md) — what the ~30s wake-recovery cascade looks like; keep dev hosts awake while baselining

## Operating principles

These are the heuristics I run the stack by:

**Treat testnet capital as real.** No cavalier flatten/close without explicit
confirm + P&L impact summary. The P&L on a $100 testnet position is fake
money; the lesson from getting reckless with it carries to the day real
capital is in play.

**Prefer clean over hybrid configs.** Don't mix testnet/mainnet via symlinks
or shared secret names. If a venue setup wants both, run two separate stacks
with separate creds.

**Failure modes that cannot happen, will happen.** Specifically: WS disconnect
storms, exchange clock skew, NTP jumps, host suspend (laptop / WSL),
mid-deploy partial state. Every one of these has happened and been
post-mortemed; the [prod-hardening backlog](https://github.com/bishanparktrading/bpt-core/blob/main/docs/backlog.md) tracks the response.

**Observability is owed before you flip a switch.** A new metric / breaker /
risk gate doesn't ship until there's a corresponding Prometheus metric +
alert rule + a manual procedure for what the operator does when it fires.
