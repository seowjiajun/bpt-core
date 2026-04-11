import { create } from 'zustand'
import type { ConnectionStatus, Msg } from './types/messages'
import type { Fill } from './components/Blotter'

interface State {
  // Session
  status: ConnectionStatus
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

  // Actions
  handleMessage: (msg: Msg) => void
  reset: () => void
}

const initialState = {
  status: 'off' as ConnectionStatus,
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
          }

        case 'status':
          return { status: msg.state }

        case 'tick': {
          const firstPrice = state.firstPrice || msg.price
          const unrealizedPnl =
            state.netQty !== 0 ? (msg.price - state.avgEntry) * state.netQty : 0
          return { price: msg.price, firstPrice, unrealizedPnl }
        }

        case 'fill': {
          const fill: Fill = {
            ts: msg.ts,
            orderId: msg.orderId,
            side: msg.side,
            qty: msg.qty,
            price: msg.price,
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
      }
    }),

  reset: () => set(initialState),
}))
