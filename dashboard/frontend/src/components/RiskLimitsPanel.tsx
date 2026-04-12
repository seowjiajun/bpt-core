import type { PortfolioGreeks } from '../types/options'

// Risk limits configuration — in production these would come from
// fenrir's strategy config via the session message or a dedicated
// limits stream. For now, hardcoded mock values.
interface RiskLimits {
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

export function RiskLimitsPanel({ greeks, limits }: Props) {
  const anyBreach = [
    utilization(greeks.netDelta, limits.maxAbsDelta),
    utilization(greeks.netGamma, limits.maxAbsGamma),
    utilization(greeks.netVega, limits.maxAbsVega),
    utilization(greeks.netTheta, limits.maxAbsTheta),
  ].some((u) => u >= 1.0)

  const lossUtilization = greeks.totalUnrealizedPnl < 0
    ? utilization(greeks.totalUnrealizedPnl, limits.maxUnrealizedLoss)
    : 0

  return (
    <div className="panel">
      <div className="panel-header">
        <span className="panel-title">Risk Limits</span>
        {anyBreach && (
          <span className="panel-badge" style={{ color: 'var(--red)', fontWeight: 700 }}>
            BREACH
          </span>
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
