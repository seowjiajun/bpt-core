import { useEffect, useRef } from 'react'
import { useStore } from '../store'
import type { PortfolioGreeks } from '../types/options'

export interface RiskLimits {
  maxAbsDelta: number
  maxAbsGamma: number
  maxAbsVega: number
  maxAbsTheta: number
  maxUnrealizedLoss: number   // max negative unrealized PnL before alert
}

interface Props {
  greeks: PortfolioGreeks
  limits: RiskLimits
}

function utilization(current: number, limit: number): number {
  if (limit === 0) return 0
  return Math.abs(current) / limit
}

function utilizationClass(pct: number): string {
  if (pct >= 1.0) return 'limit-breach'
  if (pct >= 0.8) return 'limit-warn'
  return 'limit-ok'
}

function LimitBar({ label, current, limit, decimals = 2 }: {
  label: string
  current: number
  limit: number
  decimals?: number
}) {
  const pct = utilization(current, limit)
  const cls = utilizationClass(pct)
  const barWidth = Math.min(pct * 100, 100)

  return (
    <div className="limit-row">
      <span className="limit-label">{label}</span>
      <div className="limit-bar-track">
        <div className={`limit-bar-fill ${cls}`} style={{ width: `${barWidth}%` }} />
        {pct >= 0.8 && (
          <div className="limit-bar-threshold" style={{ left: '80%' }} />
        )}
      </div>
      <span className={`limit-value ${cls}`}>
        {Math.abs(current).toFixed(decimals)} / {limit.toFixed(decimals)}
      </span>
      <span className={`limit-pct ${cls}`}>
        {(pct * 100).toFixed(0)}%
      </span>
    </div>
  )
}

function detectBreaches(greeks: PortfolioGreeks, limits: RiskLimits): string[] {
  const breaches: string[] = []
  if (utilization(greeks.netDelta, limits.maxAbsDelta) >= 1.0) breaches.push('Delta')
  if (utilization(greeks.netGamma, limits.maxAbsGamma) >= 1.0) breaches.push('Gamma')
  if (utilization(greeks.netVega, limits.maxAbsVega) >= 1.0) breaches.push('Vega')
  if (utilization(greeks.netTheta, limits.maxAbsTheta) >= 1.0) breaches.push('Theta')
  if (limits.maxUnrealizedLoss > 0 && greeks.totalUnrealizedPnl < 0 &&
      utilization(greeks.totalUnrealizedPnl, limits.maxUnrealizedLoss) >= 1.0) {
    breaches.push('Unrealized Loss')
  }
  return breaches
}

export function RiskLimitsPanel({ greeks, limits }: Props) {
  const status = useStore((s) => s.status)
  const halt = useStore((s) => s.halt)
  const autoHaltFiredRef = useRef(false)

  const breaches = detectBreaches(greeks, limits)
  const anyBreach = breaches.length > 0

  // Auto-halt: if any limit is breached and we're not already halted,
  // trigger the kill switch automatically. The ref prevents re-firing
  // on every render while halted. Resets when breaches clear so a
  // subsequent breach triggers again.
  useEffect(() => {
    if (anyBreach && status !== 'halted' && status !== 'off' && !autoHaltFiredRef.current) {
      autoHaltFiredRef.current = true
      console.warn(`[RiskLimits] AUTO-HALT triggered: ${breaches.join(', ')} breached`)
      halt()
    }
    if (!anyBreach) {
      autoHaltFiredRef.current = false
    }
  }, [anyBreach, status, halt, breaches])

  return (
    <div className="panel">
      <div className="panel-header">
        <span className="panel-title">Risk Limits</span>
        {anyBreach ? (
          <span className="panel-badge" style={{ color: 'var(--red)', fontWeight: 700 }}>
            AUTO-HALT · {breaches.join(', ')}
          </span>
        ) : (
          <span className="panel-badge" style={{ color: 'var(--green)' }}>AUTO-HALT ARMED</span>
        )}
      </div>
      <div className="panel-body" style={{ padding: '8px 12px', gap: 6, display: 'flex', flexDirection: 'column' }}>
        <LimitBar label="Delta" current={greeks.netDelta} limit={limits.maxAbsDelta} />
        <LimitBar label="Gamma" current={greeks.netGamma} limit={limits.maxAbsGamma} decimals={6} />
        <LimitBar label="Vega" current={greeks.netVega} limit={limits.maxAbsVega} decimals={1} />
        <LimitBar label="Theta" current={greeks.netTheta} limit={limits.maxAbsTheta} decimals={1} />
        {limits.maxUnrealizedLoss > 0 && (
          <LimitBar
            label="Unreal Loss"
            current={Math.min(0, greeks.totalUnrealizedPnl)}
            limit={limits.maxUnrealizedLoss}
            decimals={0}
          />
        )}
      </div>
    </div>
  )
}

// Default mock limits for development
export const MOCK_LIMITS: RiskLimits = {
  maxAbsDelta: 1.5,
  maxAbsGamma: 0.0001,
  maxAbsVega: 300,
  maxAbsTheta: 120,
  maxUnrealizedLoss: 5000,
}
