# Host sleep / WSL suspend

A laptop sleeping or a WSL2 VM idle-suspending freezes every process on the
host. When it wakes, each C++ service's Aeron client has missed its heartbeat
window and emits `timeout between service calls`. This page is what an operator
needs to know so a 30-second recovery doesn't read like an outage.

## What happens (post prod-hardening #24)

The IPC reacts deterministically — this is *designed* behaviour, not a fault:

1. On wake, `bpt-transport` (the Aeron MediaDriver) and every C++ service see the
   heartbeat gap. The client-side handler (`bpt-app`) and the transport-side
   watchdog (`MediaDriverRunner`) both `exit(1)` on `timeout between service
   calls` / `client heartbeat timestamp not active`.
2. systemd restarts `bpt-transport`. Because each dependent unit declares
   `PartOf=bpt-transport.service`, refdata / md-gw / order-gw / strategy /
   analytics / bridge **cascade-restart in lockstep**.
3. Strategy reaches `RefDataReady` and recaptures its SPOT reconcile baseline
   within a few seconds of transport's new launch.

Measured: a 90s `SIGSTOP`/`SIGCONT` of the transport JVM recovers in **<10s**.
The original cold case (overnight laptop sleep) is a ~30s recovery — the sleep
duration is dead air, then ~30s to restart + rebaseline.

## The operator-visible gap

The stack is **unreachable for the entire sleep duration + ~30s startup**, and
any open orders during that window are unmanaged until the order-gateway is back
and the strategy has rebaselined. On a dev laptop that's a nuisance; it must
never happen with real capital resting.

## Mitigations

**Dev laptop / WSL — keep it awake while baselining.** Use the helper, which
holds a `systemd-inhibit` lock until the wrapped command exits (or Ctrl-C):

```bash
scripts/dev_no_sleep.sh                 # hold until Ctrl-C
scripts/dev_no_sleep.sh ./run_stack.sh  # hold for the duration of a command
```

Or disable lid-sleep for the session. On WSL2, idle-suspend is governed by the
host Windows power plan plus `.wslconfig` — a Windows sleep suspends the VM
regardless, so the inhibitor on the Windows side is what matters there.

**Confirm recovery after a wake:**

```bash
journalctl --user -u bpt-transport -n 50        # expect exit(1) → restart
curl -s localhost:9110/metrics | grep bpt_transport_healthy          # → 1
curl -s localhost:9108/metrics | grep reconciliation_divergences     # → 0
```

**Prod — does not apply.** Trading hosts are always-on EC2; they don't lid-sleep,
and EC2 doesn't idle-suspend a running instance. (A host-down event there is a
different failure mode, covered by the Healthchecks.io dead-man's-switch in
[Monitoring](monitoring.md).) Keep the daily auto-stop schedule in mind on
cost-managed dev/QA instances — that's an intentional stop, not a suspend.
