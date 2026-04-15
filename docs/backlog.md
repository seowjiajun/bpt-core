# Backlog

Known issues and follow-ups that are not urgent enough to block current work
but should be picked up when the related subsystem is next touched.

## Hyperliquid adapter

### WS reconnect reconciliation gap

**Status:** open, observed on HL testnet 2026-04-16.

**Symptom:** HL gracefully closes the `/ws` connection every ~10 minutes
(`"The WebSocket stream was gracefully closed at both endpoints"`). Heimdall
reconnects within ~2 s, but any `userFills` events generated on the exchange
during the reconnect window are lost — HL does not replay them after the new
subscription. Fenrir's inventory then diverges from the exchange by however
many fills happened in the gap.

**Measured impact (Apr 16 session, pre partial-fill fix):** ~24 fills leaked
across 4 reconnects over ~45 min of trading, causing fenrir to underreport
inventory by ~0.024 BTC vs the real account.

**Why it's currently tolerable:** with the partial-fill + cancel-race fixes
committed today, the observable divergence in steady-state (no reconnects) is
≤ 1 fill (just the natural race between fenrir's log line and an async poll
of `/info`). The reconnect gap is the dominant remaining source of drift.

**Fix options, roughly in order of increasing scope:**

1. **Process `isSnapshot=true` fills on subscribe, dedupe by `oid`.** HL sends
   a recent-fills snapshot on subscribe. If we dedupe by the `(oid, fill_time)`
   pair against an "already seen" set, we can replay missed fills without
   double-counting the ones we already processed. Minimal new state. Simplest
   path forward.
2. **Periodic reconciliation against `clearinghouseState`.** Every ~30 s (or
   on reconnect), fetch `/info { type: "clearinghouseState" }` and compare the
   reported position vs fenrir's internal tracker. If they differ, emit a
   synthetic correction event. Wraps the WS stream with a ground-truth
   safety net. More code but catches any drift mechanism, not just WS gaps.
3. **Keep the WS alive.** HL's ~10-min grace-close cadence may be avoidable by
   a different ping pattern or reconnection strategy — worth checking once the
   account clears the scheduleCancel volume gate and we can use HL's official
   keepalive hooks. Orthogonal to 1 + 2.

Recommended first step: option (1). It's a single-file change in the parser
and directly addresses the observed leak.

**Relevant code:**
- `heimdall/src/adapter/hyperliquid/hyperliquid_ws_client.cpp` — reconnect loop
- `heimdall/src/adapter/hyperliquid/hyperliquid_exec_parser.cpp` — `handle_fills`
- Log tag to watch: `"gracefully closed at both endpoints"` in `heimdall.log`
