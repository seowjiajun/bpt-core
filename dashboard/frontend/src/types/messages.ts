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
// from bpt-order-gateway AccountSnapshots (relayed by the bridge as `account`
// messages). Backtest archive views read equity from summary.json instead.
export interface SessionMsg {
  type: 'session'
  symbol: string
  strategy: string // e.g. "MomentumStrategy" — display only
  exchange: string // e.g. "OKX" — display only
  mode: RunMode // paper | live | mock — display only
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
  ts: number // ns since epoch
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
  orderType: string // "LIMIT" | "MARKET" — POST_ONLY is now an execInst flag, not a type
  qty: number
  price: number
  fee: number // in quote currency
  realizedPnl: number
  equity: number
}

// One open position row, as reported by the exchange and relayed via
// bpt-order-gateway → bridge → this message.
export interface AccountPosition {
  symbol: string // exchange-native, e.g. "BTC"
  netQty: number // signed (+ long, − short)
  avgEntry: number // quote currency
  unrealizedPnl: number // quote currency, MTM'd by the exchange
}

// One per-currency cash balance row reported by the exchange — e.g.
// OKX accounts hold USDT + USDC + SGD + USD all at once. Distinct from
// AccountPosition (which is for non-stable crypto holdings).
export interface AccountCurrencyBalance {
  ccy: string // currency code, ≤ 8 chars (e.g. "USDT", "SGD")
  equity: number // total holdings in that currency
  availableBalance: number // withdrawable amount in that currency
}

// Live exchange account snapshot from order-gateway, relayed by the bridge.
// The dashboard uses these as the canonical equity baseline so the equity
// curve reflects the actual exchange balance, not a static config value.
// `positions` feeds crypto rows in the holdings panel; `currencyBalances`
// feeds per-stable-ccy rows.
export interface AccountMsg {
  type: 'account'
  ts: number // ns since epoch
  balance: number // quote-ccy available balance (USDT for OKX, USDC for HL)
  equity: number // total account equity (falls back to balance when 0)
  positions: AccountPosition[] // open positions with per-leg uPnL
  currencyBalances?: AccountCurrencyBalance[] // per-ccy cash rows (since v13 — may be absent on older snapshots)
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
  orderType: string // "LIMIT" | "MARKET"
  price: number
  qty: number // original order qty
  filledQty: number
  remainingQty: number
  status: OrderStatus
}

// Portfolio snapshot from bpt-strategy via the bridge — contains all option
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

// Live toxicity scores from bpt-analytics via the bridge.
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

// Live strategy state from bpt-strategy. Each strategy implementing
// IStrategy::get_strategy_state_json() emits its own shape; the `kind`
// discriminator picks the matching panel on the dashboard
// (components/panels/index.ts).
//
// To add a new strategy: add its kind below, extend StrategyStateMsg
// with a new interface, write the C++ JSON emitter, and register a
// panel component. Unknown kinds fall back to GenericStrategyPanel.
export type StrategyKind = 'AS' | 'FundingArb' | 'OptionsMaker'

interface BaseStrategyState {
  type: 'strategyState'
  kind: StrategyKind
  symbol: string
  exchange: string
}

// AvellanedaStoikov — market-making strategy with regime detection,
// queue-position-aware sizing, and vol-gate / inventory / drift /
// trend / toxicity suppression.
export interface ASStrategyState extends BaseStrategyState {
  kind: 'AS'
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

// FundingArb — cross-leg spot+perp funding-rate arbitrage. Strategy
// holds one spot leg and one perp leg in opposite directions, capturing
// the funding rate. Dashboard panel shows per-leg state + the basis
// spread that drives the entry/exit decision.
export interface FundingArbStrategyState extends BaseStrategyState {
  kind: 'FundingArb'
  spotPx: number
  perpPx: number
  basisBps: number // (perp - spot) / spot * 1e4
  fundingRate: number // annualised, decimal (0.0001 = 1 bp/funding-window)
  fundingApr: number // human-readable APR
  spotQty: number // signed; + = long spot
  perpQty: number // signed; + = long perp
  hedgedDelta: number // spotQty + perpQty; ~0 when balanced
  legStatus: 'flat' | 'entering' | 'open' | 'exiting'
}

// OptionsMakerStrategy — two-sided POST_ONLY option-chain MM with embedded
// IOC perp delta hedger. State is a list of per-(exchange, underlying)
// summaries; each carries option-side Greeks, the perp hedge position, the
// net book delta, and whether a hedge order is currently in flight.

// Per-strike row inside a per-underlying state. Included for any strike in
// the active universe OR any strike where we hold inventory (so reconciled
// orphan positions are visible even after rolling out of the universe).
export interface OptionsMakerStrike {
  strike: number
  expiry: number       // YYYYMMDD
  is_call: boolean
  theo: number         // smile-fitted BS theo in native units (BTC for Deribit)
  venue_mid: number    // 0 if no observed venue BBO (synthetic strike)
  our_bid: number      // 0 if no resting bid; else the limit price we posted
  our_ask: number
  position: number     // signed; + long
  delta: number
  vega: number
}

export interface OptionsMakerUnderlyingState {
  underlying: string
  exchange: string
  option_count: number
  portfolio_delta: number // sum of qty × delta across option positions
  portfolio_vega: number
  portfolio_gamma: number
  portfolio_theta: number
  perp_position: number   // signed; + long perp / − short
  book_delta: number      // portfolio_delta + perp_position (≈ 0 when hedged)
  hedge_in_flight: boolean
  active_strikes: OptionsMakerStrike[]
}

// OptionsMaker spans multiple (exchange, underlying) tuples concurrently, so
// the per-state-msg `symbol` / `exchange` shape that AS and FundingArb carry
// at the top level doesn't apply. The dashboard reads the per-underlying list
// directly. `risk_halted` is the global sanity-ceiling latch (set when book
// delta ever exceeds book_delta_sanity_ceiling_mult × max_hedge_abs_delta);
// when true the strategy is silently refusing to quote OR hedge until restart.
export interface OptionsMakerStrategyState {
  type: 'strategyState'
  kind: 'OptionsMaker'
  risk_halted: boolean
  underlyings: OptionsMakerUnderlyingState[]
}

export type StrategyStateMsg = ASStrategyState | FundingArbStrategyState | OptionsMakerStrategyState

// Market-color snapshot from bpt-radar via the bridge. One message per
// (exchange, underlying) every ~2s. Fields are grouped by domain so the
// wire shape can grow as new analysis modules (perp, flow, regime) come
// online without breaking consumers of the existing sections.
//
// Today only the `options` section is populated. Numeric fields can be
// `null` (encoded from C++ NaN) when the analysis module didn't have
// enough data to compute that metric — render those as "—" not "0".
export interface OptionsMarketColor {
  frontExpiry: number // YYYYMMDD
  frontTimeToExpiryY: number | null
  frontForwardPrice: number | null
  frontAtmIv: number | null
  frontRr25d: number | null
  frontSkewSlope: number | null
  backExpiry: number // YYYYMMDD
  backTimeToExpiryY: number | null
  backAtmIv: number | null
  termSpread: number | null
  gex: number | null
  maxPainStrike: number | null
  totalOi: number | null
  strikeCount: number
  expiryCount: number
  strikesWithOi: number
}

// Perp section — funding rate + next funding tick for the underlying's perp.
// Fields are null when radar hasn't joined a perp to this underlying yet
// (refdata snapshot hasn't arrived, no perp exists at this venue, etc.).
export interface PerpMarketColor {
  fundingRate8h: number | null   // decimal (e.g. 0.0001 = 1 bps)
  nextFundingTs: number | null   // ns since epoch; null = not yet known
  basisBps: number | null        // (mark - index) / index * 1e4; null if stale or missing
  markPrice: number | null       // perp mark from md-gateway InstrumentStats
  indexPrice: number | null      // index/spot from md-gateway InstrumentStats
}

// Flow section — perp-side aggressor flow over a 5min rolling window.
// All notional fields are in quote currency (USD-equivalent for crypto perps).
// Nulls when no trades observed yet for the joined perp.
export interface FlowMarketColor {
  buyNotional5m: number | null   // Σ price × qty where aggressor lifted offer
  sellNotional5m: number | null  // Σ price × qty where aggressor hit bid
  imbalance5m: number | null     // (buy − sell) / total; range [−1, +1]
  tradeCount5m: number           // # of trades inside the window
}

// Vol-regime section — annualised realized vol of the joined perp's mid over
// a 1h window. Frontend joins this with `options.frontAtmIv` to surface the
// variance risk premium and label the regime.
export interface RegimeMarketColor {
  realizedVol1h: number | null   // annualised, decimal (0.5 = 50%); null < 2 samples
  sampleCount: number            // # mid samples used
}

export interface MarketColorMsg {
  type: 'marketColor'
  ts: number // ns since epoch
  exchange: string // e.g. "DERIBIT"
  underlying: string // e.g. "BTC"
  options: OptionsMarketColor
  perp: PerpMarketColor
  flow: FlowMarketColor
  regime: RegimeMarketColor
}

export type Msg =
  | SessionMsg
  | StatusMsg
  | TickMsg
  | FillMsg
  | PositionMsg
  | OrderMsg
  | PortfolioMsg
  | AccountMsg
  | ToxicityMsg
  | MarketColorMsg
  | StrategyStateMsg
