// Opt out of HMR for this module. Zustand's store is a module-level singleton,
// so an HMR reload would create a fresh store with initialState zeros and
// every subscriber would briefly see empty/zero state until the next WS
// message repopulates. That manifested as the equity curve flipping 0↔722
// every time any unrelated component was edited. Forcing a full page reload
// on store edits is the correct behavior — you can't meaningfully hot-swap
// a state container anyway.
if (import.meta.hot) {
  import.meta.hot.decline()
}

import { create } from 'zustand'
import type {
  ConnectionStatus,
  InstrumentType,
  Msg,
  OrderMsg,
  PortfolioMsg,
  RunMode,
} from './types/messages'
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
  instrumentType: InstrumentType | null

  // Live exchange account — canonical equity source for live/paper.
  // Backtest archive views never populate these and use summary.json instead.
  accountBalance: number
  accountEquity: number
  accountHistory: Array<{ ts: number; equity: number }>
  // Open positions from the latest AccountSnapshot. Drives the holdings
  // breakdown panel. Per-position PnL here is MTM'd by the exchange.
  accountPositions: import('./types/messages').AccountPosition[]

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

  // Strategy state — updated by 'strategyState' messages from fenrir via bridge.
  strategyState: import('./types/messages').StrategyStateMsg | null

  // Tyr toxicity scores — updated by 'toxicity' messages from the bridge.
  toxicity: {
    bidMarkout5s: number
    askMarkout5s: number
    bidAdverseRate: number
    askAdverseRate: number
    bidSamples: number
    askSamples: number
    bidToxScore: number
    askToxScore: number
    bidFillRate: number
    askFillRate: number
    bidTtfMs: number
    askTtfMs: number
  } | null

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
  instrumentType: null as InstrumentType | null,
  // Live exchange account snapshots from heimdall, relayed by the bridge.
  // The EquityChart and RiskPanel use these as the canonical equity series.
  accountBalance: 0,
  accountEquity: 0,
  accountHistory: [] as Array<{ ts: number; equity: number }>,
  accountPositions: [] as State['accountPositions'],
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
  strategyState: null as State['strategyState'],
  toxicity: null as State['toxicity'],
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
            strategy: msg.strategy,
            exchange: msg.exchange,
            mode: msg.mode,
            instrumentType: msg.instrumentType ?? null,
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

        case 'account': {
          // Defensive: drop all-zero snapshots. These can leak through on a
          // failed clearinghouseState parse (heimdall catches and publishes
          // a default-constructed zero snapshot). Without this guard the
          // equity chart visibly flips to 0 on any such bad message.
          if (msg.balance === 0 && msg.equity === 0) return state
          // Append to history, but dedupe by second so multiple snapshots
          // in the same second collapse to the most recent value (otherwise
          // lightweight-charts errors on duplicate timestamps).
          const tsSec = Math.floor(msg.ts / 1_000_000_000)
          const last = state.accountHistory[state.accountHistory.length - 1]
          let history: Array<{ ts: number; equity: number }>
          if (last && Math.floor(last.ts / 1_000_000_000) === tsSec) {
            history = [...state.accountHistory.slice(0, -1), { ts: msg.ts, equity: msg.equity }]
          } else {
            history = [...state.accountHistory, { ts: msg.ts, equity: msg.equity }]
          }
          return {
            accountBalance: msg.balance,
            accountEquity: msg.equity,
            accountHistory: history,
            accountPositions: msg.positions ?? [],
          }
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

        case 'strategyState':
          return { strategyState: msg }

        case 'toxicity':
          return {
            toxicity: {
              bidMarkout5s: msg.bidMarkout5s,
              askMarkout5s: msg.askMarkout5s,
              bidAdverseRate: msg.bidAdverseRate,
              askAdverseRate: msg.askAdverseRate,
              bidSamples: msg.bidSamples,
              askSamples: msg.askSamples,
              bidToxScore: msg.bidToxScore,
              askToxScore: msg.askToxScore,
              bidFillRate: msg.bidFillRate,
              askFillRate: msg.askFillRate,
              bidTtfMs: msg.bidTtfMs,
              askTtfMs: msg.askTtfMs,
            },
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
