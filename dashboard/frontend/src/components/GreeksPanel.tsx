import type { PortfolioGreeks } from '../types/options'

interface Props {
  greeks: PortfolioGreeks
}

function greekColor(value: number, neutral = false): string {
  if (neutral) return 'stat-value--muted'
  if (value > 0) return 'stat-value--green'
  if (value < 0) return 'stat-value--red'
  return 'stat-value--muted'
}

function fmtGreek(value: number, decimals = 2): string {
  const sign = value >= 0 ? '+' : ''
  return `${sign}${value.toFixed(decimals)}`
}

export function GreeksPanel({ greeks }: Props) {
  const pnlColor = greeks.totalUnrealizedPnl >= 0 ? 'stat-value--green' : 'stat-value--red'

  return (
    <div className="panel">
      <div className="panel-header">
        <span className="panel-title">Portfolio Greeks</span>
      </div>
      <div className="stat-grid">
        <div className="stat-cell">
          <span className="stat-label">Delta</span>
          <span className={`stat-value ${greekColor(greeks.netDelta)}`}>
            {fmtGreek(greeks.netDelta)}
          </span>
        </div>
        <div className="stat-cell">
          <span className="stat-label">Gamma</span>
          <span className={`stat-value stat-value--sm ${greekColor(greeks.netGamma)}`}>
            {fmtGreek(greeks.netGamma, 6)}
          </span>
        </div>
        <div className="stat-cell">
          <span className="stat-label">Vega</span>
          <span className={`stat-value stat-value--sm ${greekColor(greeks.netVega)}`}>
            {fmtGreek(greeks.netVega, 1)}
          </span>
        </div>
        <div className="stat-cell">
          <span className="stat-label">Theta</span>
          <span className={`stat-value stat-value--sm ${greekColor(greeks.netTheta)}`}>
            {fmtGreek(greeks.netTheta, 1)}
          </span>
        </div>
        <div className="stat-cell">
          <span className="stat-label">Unreal PnL</span>
          <span className={`stat-value ${pnlColor}`}>
            {greeks.totalUnrealizedPnl >= 0 ? '+' : ''}${greeks.totalUnrealizedPnl.toFixed(2)}
          </span>
        </div>
        <div className="stat-cell">
          <span className="stat-label">Real PnL</span>
          <span className={`stat-value stat-value--sm stat-value--muted`}>
            {greeks.totalRealizedPnl >= 0 ? '+' : ''}${greeks.totalRealizedPnl.toFixed(2)}
          </span>
        </div>
      </div>
    </div>
  )
}
