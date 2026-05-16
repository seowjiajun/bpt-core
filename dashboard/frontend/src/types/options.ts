// Options data types — mirrors the shapes from bpt-pricer's VolSurface grid and
// bpt-strategy's refdata instruments. Mock data uses these directly; phase 2 will
// wire them to a real PortfolioSnapshot Aeron stream via the bridge.

export type OptionSide = 'CALL' | 'PUT'

// One leg of an options portfolio — a single instrument position with
// Greeks snapshot from the latest vol surface update.
export interface OptionLeg {
  instrumentId: number
  symbol: string // e.g. "BTC-20260620-100000-C"
  underlying: string // e.g. "BTC-USDT"
  strike: number
  expiry: number // YYYYMMDD
  optionSide: OptionSide
  qty: number // +ve = long, -ve = short
  avgEntry: number // average fill price
  markPrice: number // current market mid
  iv: number // implied vol (annualized)
  // Per-leg Greeks (from bpt-pricer, per 1 contract)
  delta: number
  gamma: number
  vega: number
  theta: number
  unrealizedPnl: number
}

// Portfolio-level aggregated Greeks — sum across all legs.
export interface PortfolioGreeks {
  netDelta: number
  netGamma: number
  netVega: number
  netTheta: number
  totalUnrealizedPnl: number
  totalRealizedPnl: number
}

// A single point on the vol surface — matches bpt-pricer's VolSurface grid point.
export interface VolSurfacePoint {
  instrumentId: number
  underlying: string // "BTC", "ETH", etc. — required for multi-underlying strategies
  strike: number
  expiry: number // YYYYMMDD
  optionSide: OptionSide
  iv: number // mid IV
  bidIv: number
  askIv: number
  delta: number
  timeToExpiry: number // years
}

// Grouped by (underlying, expiry) for rendering smile curves. One slice
// belongs to exactly one underlying — charts filter by selected underlying
// before processing.
export interface VolSmileSlice {
  underlying: string
  expiry: number // YYYYMMDD
  label: string // e.g. "20 Jun"
  daysToExpiry: number
  points: VolSurfacePoint[]
}
