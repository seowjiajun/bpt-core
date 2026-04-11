// WebSocket message schema — the contract between the bridge and the frontend.
//
// The mock replay (src/mock/replay.ts) emits exactly these messages so the
// components never know whether they're seeing real Aeron data or synthetic
// playback.  The real C++ bridge will emit the same JSON.

export type ConnectionStatus = 'live' | 'mock' | 'halted' | 'off'

export type Side = 'BUY' | 'SELL'

// Sent once at the start of a session (and replayed to mid-run joiners).
export interface SessionMsg {
  type: 'session'
  symbol: string
  startingCapital: number
  strategy: string    // e.g. "MomentumStrategy" — display only
  exchange: string    // e.g. "OKX" — display only
}

// Connection / run state.
export interface StatusMsg {
  type: 'status'
  state: ConnectionStatus
}

// Market data tick.  Emitted frequently — top-bar price comes from these.
export interface TickMsg {
  type: 'tick'
  ts: number        // ns since epoch
  symbol: string
  price: number
}

// One fill from the exchange — drives the blotter, equity curve, chart markers.
export interface FillMsg {
  type: 'fill'
  ts: number
  orderId: number
  symbol: string
  side: Side
  qty: number
  price: number
  realizedPnl: number
  equity: number
}

// Current net position.  Emitted by the bridge after every fill.
export interface PositionMsg {
  type: 'position'
  symbol: string
  netQty: number
  avgEntry: number
  unrealizedPnl: number
}

export type Msg = SessionMsg | StatusMsg | TickMsg | FillMsg | PositionMsg
