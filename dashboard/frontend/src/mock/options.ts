// Mock options data for dashboard development. Simulates a short-vol
// BTC options portfolio: short strangles at multiple expiries, hedged
// with a small delta position in the underlying perp.
//
// All values are realistic for BTC options circa Jan 2026 (spot ~$92K).

import type { OptionLeg, VolSmileSlice, VolSurfacePoint, PortfolioGreeks } from '../types/options'

const SPOT = 92_000

// ── Portfolio legs ───────────────────────────────────────────────────────────
// Short strangle: sell OTM call + sell OTM put at two expiries.
export const MOCK_LEGS: OptionLeg[] = [
  // Near-term strangle (7 DTE)
  {
    instrumentId: 1001,
    symbol: 'BTC-20260113-95000-C',
    underlying: 'BTC-USDT',
    strike: 95_000,
    expiry: 20260113,
    optionSide: 'CALL',
    qty: -2,
    avgEntry: 1_420,
    markPrice: 1_180,
    iv: 0.62,
    delta: -0.32,
    gamma: -0.000018,
    vega: -42.5,
    theta: 28.3,
    unrealizedPnl: 480,
  },
  {
    instrumentId: 1002,
    symbol: 'BTC-20260113-88000-P',
    underlying: 'BTC-USDT',
    strike: 88_000,
    expiry: 20260113,
    optionSide: 'PUT',
    qty: -2,
    avgEntry: 980,
    markPrice: 720,
    iv: 0.68,
    delta: 0.24,
    gamma: -0.000015,
    vega: -38.1,
    theta: 25.7,
    unrealizedPnl: 520,
  },
  // Far-term strangle (30 DTE)
  {
    instrumentId: 2001,
    symbol: 'BTC-20260206-100000-C',
    underlying: 'BTC-USDT',
    strike: 100_000,
    expiry: 20260206,
    optionSide: 'CALL',
    qty: -1,
    avgEntry: 3_200,
    markPrice: 2_850,
    iv: 0.58,
    delta: -0.25,
    gamma: -0.000008,
    vega: -82.3,
    theta: 12.1,
    unrealizedPnl: 350,
  },
  {
    instrumentId: 2002,
    symbol: 'BTC-20260206-82000-P',
    underlying: 'BTC-USDT',
    strike: 82_000,
    expiry: 20260206,
    optionSide: 'PUT',
    qty: -1,
    avgEntry: 2_100,
    markPrice: 1_920,
    iv: 0.65,
    delta: 0.18,
    gamma: -0.000006,
    vega: -75.8,
    theta: 10.4,
    unrealizedPnl: 180,
  },
]

// ── Aggregated Greeks ────────────────────────────────────────────────────────
function computeGreeks(legs: OptionLeg[]): PortfolioGreeks {
  let netDelta = 0,
    netGamma = 0,
    netVega = 0,
    netTheta = 0
  let totalUnrealizedPnl = 0,
    totalRealizedPnl = 0
  for (const l of legs) {
    const absQty = Math.abs(l.qty)
    netDelta += l.delta * absQty
    netGamma += l.gamma * absQty
    netVega += l.vega * absQty
    netTheta += l.theta * absQty
    totalUnrealizedPnl += l.unrealizedPnl
  }
  return { netDelta, netGamma, netVega, netTheta, totalUnrealizedPnl, totalRealizedPnl }
}

export const MOCK_GREEKS: PortfolioGreeks = computeGreeks(MOCK_LEGS)

// ── Vol surface ──────────────────────────────────────────────────────────────
// Smile for two expiries, 9 strikes each.
function smile(expiry: number, daysToExpiry: number, label: string): VolSmileSlice {
  const tte = daysToExpiry / 365
  const strikes = [78_000, 82_000, 86_000, 88_000, 92_000, 95_000, 98_000, 100_000, 105_000]
  const baseIv = 0.55 + 0.08 / Math.sqrt(daysToExpiry / 7)

  const points: VolSurfacePoint[] = []
  for (const strike of strikes) {
    const moneyness = Math.log(strike / SPOT)
    // Skew: puts have higher IV (typical crypto smile with left skew)
    const skew = -0.15 * moneyness + 0.8 * moneyness * moneyness
    const iv = baseIv + skew
    const spread = iv * 0.04

    // Rough Black-Scholes delta approximation
    const d1 = (Math.log(SPOT / strike) + 0.5 * iv * iv * tte) / (iv * Math.sqrt(tte))
    const callDelta = normalCdf(d1)

    points.push({
      instrumentId: expiry * 1000 + Math.round(strike / 1000),
      strike,
      expiry,
      optionSide: 'CALL',
      iv,
      bidIv: iv - spread / 2,
      askIv: iv + spread / 2,
      delta: callDelta,
      timeToExpiry: tte,
    })
    points.push({
      instrumentId: expiry * 1000 + Math.round(strike / 1000) + 500,
      strike,
      expiry,
      optionSide: 'PUT',
      iv: iv + 0.02,
      bidIv: iv + 0.02 - spread / 2,
      askIv: iv + 0.02 + spread / 2,
      delta: callDelta - 1,
      timeToExpiry: tte,
    })
  }

  return { expiry, label, daysToExpiry, points }
}

function normalCdf(x: number): number {
  const t = 1 / (1 + 0.2316419 * Math.abs(x))
  const d = 0.3989422802 * Math.exp(-0.5 * x * x)
  const p = d * t * (0.3193815 + t * (-0.3565638 + t * (1.781478 + t * (-1.821256 + t * 1.330274))))
  return x > 0 ? 1 - p : p
}

export const MOCK_VOL_SURFACE: VolSmileSlice[] = [
  smile(20260113, 7, '13 Jan'),
  smile(20260206, 30, '06 Feb'),
]

export { SPOT as MOCK_SPOT }
