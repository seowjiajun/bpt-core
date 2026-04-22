// WebSocket message schema — the contract between the bridge and the frontend.
//
// The mock replay (src/mock/replay.ts) emits exactly these messages so the
// components never know whether they're seeing real Aeron data or synthetic
// playback.  The real C++ bridge will emit the same JSON.

export type ConnectionStatus = 'live' | 'mock' | 'halted' | 'off'

// Run mode — distinct from connection state.  Drives the prominent
// pill in the top bar so the user can tell at a glance whether they
// are looking at simulated trading or real money.  Backtest is NOT a
// valid live console mode — completed backtests live in /archive.
export type RunMode = 'paper' | 'live' | 'mock'

export type Side = 'BUY' | 'SELL'

// Instrument type — display only.  Bridge passes it through from config
// or --instrument-type CLI flag; used to label the top bar so traders can
// tell perp/future/option from spot at a glance.
export type InstrumentType = 'SPOT' | 'PERP' | 'FUTURE' | 'OPTION'

// Sent once at the start of a session (and replayed to mid-run joiners).
//
// No starting_capital field — equity for live/paper sessions is sourced
// from heimdall AccountSnapshots (relayed by the bridge as `account`
// messages). Backtest archive views read equity from summary.json instead.
export interface SessionMsg {
  type: 'session'
  symbol: string
  strategy: string              // e.g. "MomentumStrategy" — display only
  exchange: string              // e.g. "OKX" — display only
  mode: RunMode                 // paper | live | mock — display only
  instrumentType?: InstrumentType // SPOT/PERP/FUTURE/OPTION — display only
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
  orderType: string   // "LIMIT" | "MARKET" | "POST_ONLY"
  qty: number
  price: number
  fee: number         // in quote currency
  realizedPnl: number
  equity: number
}

// One open position row, as reported by the exchange and relayed via
// heimdall → bridge → this message.
export interface AccountPosition {
  symbol: string          // exchange-native, e.g. "BTC"
  netQty: number          // signed (+ long, − short)
  avgEntry: number        // quote currency
  unrealizedPnl: number   // quote currency, MTM'd by the exchange
}

// One per-currency cash balance row reported by the exchange — e.g.
// OKX accounts hold USDT + USDC + SGD + USD all at once. Distinct from
// AccountPosition (which is for non-stable crypto holdings).
export interface AccountCurrencyBalance {
  ccy: string                       // currency code, ≤ 8 chars (e.g. "USDT", "SGD")
  equity: number                    // total holdings in that currency
  availableBalance: number          // withdrawable amount in that currency
}

// Live exchange account snapshot from order-gateway, relayed by the bridge.
// The dashboard uses these as the canonical equity baseline so the equity
// curve reflects the actual exchange balance, not a static config value.
// `positions` feeds crypto rows in the holdings panel; `currencyBalances`
// feeds per-stable-ccy rows.
export interface AccountMsg {
  type: 'account'
  ts: number                        // ns since epoch
  balance: number                   // quote-ccy available balance (USDT for OKX, USDC for HL)
  equity: number                    // total account equity (falls back to balance when 0)
  positions: AccountPosition[]      // open positions with per-leg uPnL
  currencyBalances?: AccountCurrencyBalance[]  // per-ccy cash rows (since v13 — may be absent on older snapshots)
}

// Current net position.  Emitted by the bridge after every fill.
export interface PositionMsg {
  type: 'position'
  symbol: string
  netQty: number
  avgEntry: number
  unrealizedPnl: number
}

// Order lifecycle event.  Emitted for every exec report status:
// acked → working order, partial → partially filled, filled → done,
// cancelled → removed, rejected → never entered the book.
export type OrderStatus = 'acked' | 'partial' | 'filled' | 'cancelled' | 'rejected'

export interface OrderMsg {
  type: 'order'
  ts: number
  orderId: number
  symbol: string
  side: Side
  orderType: string     // "LIMIT" | "MARKET"
  price: number
  qty: number           // original order qty
  filledQty: number
  remainingQty: number
  status: OrderStatus
}

// Portfolio snapshot from fenrir via the bridge — contains all option
// legs with Greeks, aggregated portfolio Greeks, and vol surface points.
// Published at ~10Hz when an options strategy is running.
export interface PortfolioMsg {
  type: 'portfolio'
  ts: number
  delta: number
  gamma: number
  vega: number
  theta: number
  unrealizedPnl: number
  realizedPnl: number
  legs: Array<{
    instrumentId: number
    symbol: string
    underlying: string
    expiry: number
    strike: number
    isCall: boolean
    isOption: boolean
    qty: number
    entryPrice: number
    markPrice: number
    iv: number
    delta: number
    gamma: number
    vega: number
    theta: number
    unrealizedPnl: number
  }>
  surface: Array<{
    instrumentId: number
    expiry: number
    strike: number
    isCall: boolean
    iv: number
    bidIv: number
    askIv: number
    delta: number
    tte: number
  }>
}

// Live toxicity scores from Tyr via the bridge.
export interface ToxicityMsg {
  type: 'toxicity'
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
}

// Live strategy state from fenrir — AS model parameters, suppression state,
// inventory, vol gate status. Published at ~2 Hz.
export interface StrategyStateMsg {
  type: 'strategyState'
  symbol: string
  exchange: string
  drift: number
  driftBps: number
  // Slow-drift signal: cumulative return (in bps) from a rolling
  // anchor advanced every slow_drift_window_s. Complements `driftBps`
  // which is the per-√s EWMA — fast signal vs trend signal. Side
  // suppression triggers when |slowDriftBps| > slowDriftSuppressBps
  // (0 = feature disabled in strategy config).
  slowDriftBps?: number
  slowDriftSuppressBps?: number
  sigma2: number
  kappa: number
  kappaLive: boolean
  mid: number
  reservation: number
  reservationOffsetBps: number
  halfSpread: number
  halfSpreadBps: number
  inventory: number
  maxInventory: number
  inventoryPct: number
  bidSuppressed: boolean
  bidSuppressReason: string
  askSuppressed: boolean
  askSuppressReason: string
  volGateHalted: boolean
  volGateTrips: number
  bidOrderLive: boolean
  askOrderLive: boolean
  bidPrice: number
  askPrice: number
  volTicks: number
  volWarmup: number
  warmedUp: boolean
  regime: string
  hurst: number
  gammaBase: number
  gammaEffective: number
  gammaMultiplier: number
  // Queue-position state (added when L2 depth consumption landed).
  // bookBidLevels/bookAskLevels: live ladder depth — 0 means the book
  //   hasn't warmed up yet (early frames arrive before the instrument
  //   snapshot is resolved).
  // bidQueueAhead/askQueueAhead: total qty sitting in front of our
  //   resting order at its price, in base units. 0 when no order is live.
  // bidFillProb/askFillProb: actual fill-probability proxy for the
  //   resting order = our_qty / (our_qty + queue_ahead).
  // bidProjectedFillProb/askProjectedFillProb: same ratio but computed
  //   at the *candidate* quote price on the current tick — what the
  //   strategy WOULD see if it placed now.
  // queueSuppressMin: config floor; a side is queue-suppressed when its
  //   projected fill prob dips below this.
  bookBidLevels?: number
  bookAskLevels?: number
  bidQueueAhead?: number
  askQueueAhead?: number
  bidFillProb?: number
  askFillProb?: number
  bidProjectedFillProb?: number
  askProjectedFillProb?: number
  queueSuppressMin?: number
  // Market best bid/ask — for the PriceChart overlay so users can
  // see our quote placement relative to the touch. 0 when the book
  // hasn't warmed up.
  marketBid?: number
  marketAsk?: number
}

export type Msg = SessionMsg | StatusMsg | TickMsg | FillMsg | PositionMsg | OrderMsg | PortfolioMsg | AccountMsg | ToxicityMsg | StrategyStateMsg
