import { create } from 'zustand'
import type { ConnectionStatus, Msg, OrderMsg, PortfolioMsg, RunMode } from './types/messages'
import type { Fill } from './components/Blotter'
import type { OptionLeg, PortfolioGreeks, VolSmileSlice, VolSurfacePoint } from './types/options'
import { sendCommand } from './ws/client'

// Working order tracked by the store — derived from OrderMsg events.
export interface OpenOrder {
  orderId: number
  symbol: string
  side: 'BUY' | 'SELL'
  orderType: string
  price: number
  qty: number
  filledQty: number
  remainingQty: number
  status: OrderMsg['status']
  ts: number
}

// OHLC bar for the price chart. Built client-side by rolling incoming
// TickMsg prices into 1-minute buckets — the bridge doesn't push candles
// today, and doing the rollup here means we don't need a new message
// type on the wire.  Keep it in sync with lightweight-charts' time unit
// (unix seconds, not nanos).
export interface Candle {
  time: number    // unix seconds
  open: number
  high: number
  low: number
  close: number
}

// Seconds per bar.  1 minute is the conventional default for a live
// console; parameterize only if users start asking for it.
const BAR_SEC = 60
// Cap in-memory candle history.  The chart only ever shows the last
// few hundred bars anyway; unbounded growth is a leak.
const MAX_CANDLES = 600

interface State {
  // Session
  status: ConnectionStatus
  mode: RunMode
  symbol: string
  strategy: string
  exchange: string
  startingCapital: number

  // Market
  firstPrice: number     // set on the first tick; used for top-bar % change
  price: number

  // Position (bridge-provided — bridge is the source of truth)
  netQty: number
  avgEntry: number
  unrealizedPnl: number

  // Fills
  fills: Fill[]

  // OHLC candles rolled up from incoming ticks. See BAR_SEC / MAX_CANDLES.
  candles: Candle[]

  // Open/working orders keyed by orderId. Orders appear on ACKED,
  // update on PARTIAL, and are removed on FILLED/CANCELLED/REJECTED.
  openOrders: Map<number, OpenOrder>

  // Options portfolio state — populated by 'portfolio' messages from
  // fenrir via the bridge. Null when no options strategy is running.
  optionLegs: OptionLeg[]
  portfolioGreeks: PortfolioGreeks | null
  volSurface: VolSmileSlice[]

  // Kill-switch state: tracks the previous status so resume() can restore
  // the connection dot to whatever it was before the halt.  In slice (a)
  // this is pure client state; slice (b) will drive it from server acks.
  preHaltStatus: ConnectionStatus | null

  // Actions
  handleMessage: (msg: Msg) => void
  reset: () => void

  // Kill switch — sends a command to the bridge via WS. The bridge
  // broadcasts a status message back to all clients, which handleMessage
  // picks up to flip the UI. In mock mode (no WS), falls back to a local
  // optimistic mutation.
  halt: () => void
  resume: () => void
}

const initialState = {
  status: 'off' as ConnectionStatus,
  mode: 'mock' as RunMode,
  symbol: '',
  strategy: '',
  exchange: '',
  startingCapital: 0,
  firstPrice: 0,
  price: 0,
  netQty: 0,
  avgEntry: 0,
  unrealizedPnl: 0,
  fills: [] as Fill[],
  candles: [] as Candle[],
  openOrders: new Map<number, OpenOrder>(),
  optionLegs: [] as OptionLeg[],
  portfolioGreeks: null as PortfolioGreeks | null,
  volSurface: [] as VolSmileSlice[],
  preHaltStatus: null as ConnectionStatus | null,
}

export const useStore = create<State>((set) => ({
  ...initialState,

  handleMessage: (msg) =>
    set((state) => {
      switch (msg.type) {
        case 'session':
          return {
            symbol: msg.symbol,
            startingCapital: msg.startingCapital,
            strategy: msg.strategy,
            exchange: msg.exchange,
            mode: msg.mode,
          }

        case 'status':
          return { status: msg.state }

        case 'tick': {
          const firstPrice = state.firstPrice || msg.price
          const unrealizedPnl =
            state.netQty !== 0 ? (msg.price - state.avgEntry) * state.netQty : 0

          // Roll this tick into the current 1m bar, or start a new bar
          // if the tick crossed a bucket boundary.  Time is in seconds
          // because lightweight-charts uses UTCTimestamp (unix seconds).
          const tsSec = Math.floor(msg.ts / 1_000_000_000)
          const bucketStart = Math.floor(tsSec / BAR_SEC) * BAR_SEC
          const last = state.candles[state.candles.length - 1]

          let candles: Candle[]
          if (!last || last.time < bucketStart) {
            const fresh: Candle = {
              time: bucketStart,
              open: msg.price,
              high: msg.price,
              low:  msg.price,
              close: msg.price,
            }
            candles = [...state.candles, fresh]
            if (candles.length > MAX_CANDLES) candles = candles.slice(-MAX_CANDLES)
          } else {
            // Still inside the same bucket — update high/low/close in place.
            const updated: Candle = {
              ...last,
              high:  msg.price > last.high ? msg.price : last.high,
              low:   msg.price < last.low  ? msg.price : last.low,
              close: msg.price,
            }
            candles = [...state.candles.slice(0, -1), updated]
          }

          return { price: msg.price, firstPrice, unrealizedPnl, candles }
        }

        case 'fill': {
          const fill: Fill = {
            seq: state.fills.length,  // monotonic, unique per store instance
            ts: msg.ts,
            orderId: msg.orderId,
            side: msg.side,
            orderType: msg.orderType,
            qty: msg.qty,
            price: msg.price,
            fee: msg.fee,
            realizedPnl: msg.realizedPnl,
            equity: msg.equity,
          }
          return { fills: [...state.fills, fill] }
        }

        case 'position':
          return {
            netQty: msg.netQty,
            avgEntry: msg.avgEntry,
            unrealizedPnl: msg.unrealizedPnl,
          }

        case 'portfolio': {
          const legs: OptionLeg[] = msg.legs
            .filter((l) => l.isOption)
            .map((l, i) => ({
              instrumentId: l.instrumentId,
              symbol: l.symbol,
              underlying: l.underlying,
              strike: l.strike,
              expiry: l.expiry,
              optionSide: l.isCall ? 'CALL' as const : 'PUT' as const,
              qty: l.qty,
              avgEntry: l.entryPrice,
              markPrice: l.markPrice,
              iv: l.iv,
              delta: l.delta,
              gamma: l.gamma,
              vega: l.vega,
              theta: l.theta,
              unrealizedPnl: l.unrealizedPnl,
            }))

          const greeks: PortfolioGreeks = {
            netDelta: msg.delta,
            netGamma: msg.gamma,
            netVega: msg.vega,
            netTheta: msg.theta,
            totalUnrealizedPnl: msg.unrealizedPnl,
            totalRealizedPnl: msg.realizedPnl,
          }

          // Group surface points by expiry into VolSmileSlice[]
          const byExpiry = new Map<number, VolSurfacePoint[]>()
          for (const sp of msg.surface) {
            const pts = byExpiry.get(sp.expiry) ?? []
            pts.push({
              instrumentId: sp.instrumentId,
              strike: sp.strike,
              expiry: sp.expiry,
              optionSide: sp.isCall ? 'CALL' : 'PUT',
              iv: sp.iv,
              bidIv: sp.bidIv,
              askIv: sp.askIv,
              delta: sp.delta,
              timeToExpiry: sp.tte,
            })
            byExpiry.set(sp.expiry, pts)
          }
          const surface: VolSmileSlice[] = [...byExpiry.entries()]
            .sort(([a], [b]) => a - b)
            .map(([expiry, points]) => {
              const tte = points[0]?.timeToExpiry ?? 0
              const dte = Math.round(tte * 365)
              const d = String(expiry)
              const label = `${d.slice(6, 8)} ${['', 'Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'][Number(d.slice(4, 6))] ?? d.slice(4, 6)}`
              return { expiry, label, daysToExpiry: dte, points }
            })

          return { optionLegs: legs, portfolioGreeks: greeks, volSurface: surface }
        }

        case 'order': {
          const next = new Map(state.openOrders)
          if (msg.status === 'filled' || msg.status === 'cancelled' || msg.status === 'rejected') {
            next.delete(msg.orderId)
          } else {
            next.set(msg.orderId, {
              orderId: msg.orderId,
              symbol: msg.symbol,
              side: msg.side,
              orderType: msg.orderType,
              price: msg.price,
              qty: msg.qty,
              filledQty: msg.filledQty,
              remainingQty: msg.remainingQty,
              status: msg.status,
              ts: msg.ts,
            })
          }
          return { openOrders: next }
        }
      }
    }),

  reset: () => set(initialState),

  halt: () => {
    const { status } = useStore.getState()
    if (status === 'halted') return
    if (status === 'mock') {
      // Mock mode: no bridge, just flip locally
      set({ preHaltStatus: status, status: 'halted' })
    } else {
      // Real mode: send command, let the bridge's status broadcast update UI
      set({ preHaltStatus: status })
      sendCommand('halt')
    }
  },

  resume: () => {
    const { status, preHaltStatus } = useStore.getState()
    if (status !== 'halted') return
    if (preHaltStatus === 'mock') {
      set({ status: 'mock', preHaltStatus: null })
    } else {
      sendCommand('resume')
    }
  },
}))
