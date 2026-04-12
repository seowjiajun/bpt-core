// Mock replay — fires the WebSocket message schema on a timer so the dashboard
// animates as if a real bridge were connected.  Emits the same JSON shape that
// the real C++ bridge will emit, so components don't need a code path switch.

import type { Fill } from '../components/Blotter'
import type { Msg, Side } from '../types/messages'
import { useStore } from '../store'

// Minimal mock-fill shape — orderType and fee are synthesized by the replay.
export type MockFill = Omit<Fill, 'seq' | 'orderType' | 'fee'>

interface ReplayConfig {
  fills: MockFill[]
  symbol: string
  strategy: string
  exchange: string
  startingCapital: number
  intervalMs?: number   // time between fills (default 1200)
  initialDelayMs?: number
}

// Mock mode always emits mode="mock" — it's frontend-only fake data, never
// a real backtest / paper / live run.

// Position tracker — mirrors what the bridge will compute server-side.
class PositionTracker {
  netQty = 0
  avgEntry = 0

  apply(side: Side, qty: number, price: number) {
    const signed = side === 'BUY' ? qty : -qty
    const prev = this.netQty
    const next = prev + signed

    if (next === 0) {
      this.avgEntry = 0
    } else if (prev === 0 || Math.sign(prev) !== Math.sign(next)) {
      // Opening fresh or flipping sides
      this.avgEntry = price
    } else if (Math.abs(next) > Math.abs(prev)) {
      // Adding to an existing position — weighted average
      this.avgEntry = (this.avgEntry * Math.abs(prev) + price * qty) / Math.abs(next)
    }
    // Reducing: keep avgEntry unchanged
    this.netQty = next
  }
}

export function startMockReplay(cfg: ReplayConfig): () => void {
  const { fills, symbol, strategy, exchange, startingCapital, intervalMs = 1200, initialDelayMs = 500 } = cfg
  const dispatch = (msg: Msg) => useStore.getState().handleMessage(msg)

  useStore.getState().reset()
  dispatch({ type: 'session', symbol, startingCapital, strategy, exchange, mode: 'mock' })
  dispatch({ type: 'status', state: 'mock' })

  const tracker = new PositionTracker()
  const timeouts: number[] = []

  // Seed tick at the starting price so the top bar doesn't show 0.
  if (fills.length > 0) {
    const firstTs = fills[0].ts - 1_000_000_000
    timeouts.push(
      window.setTimeout(
        () => dispatch({ type: 'tick', ts: firstTs, symbol, price: fills[0].price }),
        0,
      ),
    )
  }

  // Emit synthetic in-between ticks so the candle rollup in the store
  // produces a lively chart during mock replay.  Each pair of consecutive
  // fills gets N ticks spread linearly across both wall-clock (for the UI
  // animation) and sim-clock (so the chart bucketing is sensible), with a
  // small random walk that drifts toward the next fill price.
  const TICKS_PER_FILL = 8
  for (let i = 0; i < fills.length - 1; i++) {
    const a = fills[i]
    const b = fills[i + 1]
    const baseWallMs = initialDelayMs + i * intervalMs
    const dWallMs = intervalMs
    const dSimNs = b.ts - a.ts
    const dPrice = b.price - a.price

    for (let k = 1; k < TICKS_PER_FILL; k++) {
      const frac = k / TICKS_PER_FILL
      const simTs = a.ts + Math.floor(dSimNs * frac)
      const noise = (Math.random() - 0.5) * Math.abs(a.price) * 0.0008
      const price = a.price + dPrice * frac + noise
      const wallDelay = Math.floor(baseWallMs + dWallMs * frac)
      timeouts.push(
        window.setTimeout(
          () => dispatch({ type: 'tick', ts: simTs, symbol, price }),
          wallDelay,
        ),
      )
    }
  }

  fills.forEach((fill, i) => {
    // Emit an ACKED order event slightly before the fill so the open
    // orders panel shows orders entering the book and then filling.
    const ackDelay = initialDelayMs + i * intervalMs - 200
    if (ackDelay > 0) {
      timeouts.push(
        window.setTimeout(() => {
          dispatch({
            type: 'order',
            ts: fill.ts - 200_000_000,
            orderId: fill.orderId,
            symbol,
            side: fill.side,
            orderType: 'LIMIT',
            price: fill.price,
            qty: fill.qty,
            filledQty: 0,
            remainingQty: fill.qty,
            status: 'acked',
          })
        }, ackDelay),
      )
    }

    const delay = initialDelayMs + i * intervalMs
    const id = window.setTimeout(() => {
      // Tick first (price moves to the fill price)
      dispatch({ type: 'tick', ts: fill.ts, symbol, price: fill.price })

      // Order filled event — removes from open orders
      dispatch({
        type: 'order',
        ts: fill.ts,
        orderId: fill.orderId,
        symbol,
        side: fill.side,
        orderType: 'LIMIT',
        price: fill.price,
        qty: fill.qty,
        filledQty: fill.qty,
        remainingQty: 0,
        status: 'filled',
      })

      // Then the fill itself — mock is all LIMIT with a token maker fee
      dispatch({
        type: 'fill',
        ts: fill.ts,
        orderId: fill.orderId,
        symbol,
        side: fill.side,
        orderType: 'LIMIT',
        qty: fill.qty,
        price: fill.price,
        fee: fill.qty * fill.price * 0.0002,  // 2 bps taker-ish placeholder
        realizedPnl: fill.realizedPnl,
        equity: fill.equity,
      })

      // Then the updated position
      tracker.apply(fill.side, fill.qty, fill.price)
      const unrealizedPnl =
        tracker.netQty !== 0 ? (fill.price - tracker.avgEntry) * tracker.netQty : 0
      dispatch({
        type: 'position',
        symbol,
        netQty: tracker.netQty,
        avgEntry: tracker.avgEntry,
        unrealizedPnl,
      })

      // After the last fill, mark the session complete.
      if (i === fills.length - 1) {
        const doneId = window.setTimeout(
          () => dispatch({ type: 'status', state: 'off' }),
          1500,
        )
        timeouts.push(doneId)
      }
    }, delay)
    timeouts.push(id)
  })

  // Cleanup handle
  return () => {
    timeouts.forEach((t) => clearTimeout(t))
  }
}
