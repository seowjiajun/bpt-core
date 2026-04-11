// Mock replay — fires the WebSocket message schema on a timer so the dashboard
// animates as if a real bridge were connected.  Emits the same JSON shape that
// the real C++ bridge will emit, so components don't need a code path switch.

import type { Fill } from '../components/Blotter'
import type { Msg, Side } from '../types/messages'
import { useStore } from '../store'

interface ReplayConfig {
  fills: Fill[]
  symbol: string
  strategy: string
  exchange: string
  startingCapital: number
  intervalMs?: number   // time between fills (default 1200)
  initialDelayMs?: number
}

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
  dispatch({ type: 'session', symbol, startingCapital, strategy, exchange })
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

  fills.forEach((fill, i) => {
    const delay = initialDelayMs + i * intervalMs
    const id = window.setTimeout(() => {
      // Tick first (price moves to the fill price)
      dispatch({ type: 'tick', ts: fill.ts, symbol, price: fill.price })

      // Then the fill itself
      dispatch({
        type: 'fill',
        ts: fill.ts,
        orderId: fill.orderId,
        symbol,
        side: fill.side,
        qty: fill.qty,
        price: fill.price,
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
