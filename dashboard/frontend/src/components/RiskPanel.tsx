import { useStore } from '../store'
import type { Fill } from './Blotter'

interface RiskPanelProps {
  // Optional overrides for the archive view.
  fills?: Fill[]
  startingCapital?: number
  // When rendering an archive run we have these pre-computed from summary.json
  // and don't need to recompute client-side.
  precomputed?: {
    totalPnl: number
    returnPct: number
    maxDdPct: number
    sharpe: number
    winRate: number
    totalFills: number
  }
}

export function RiskPanel(props: RiskPanelProps = {}) {
  const liveFills = useStore((s) => s.fills)
  const liveCap = useStore((s) => s.startingCapital)
  const fills = props.fills ?? liveFills
  const startingCapital = props.startingCapital ?? liveCap

  // All risk metrics are derived from the fill stream, not pushed from the
  // bridge — this keeps the bridge's responsibility surface small.
  // If precomputed metrics are supplied (archive view from summary.json),
  // use them directly instead of re-deriving from the fill stream.
  let totalPnl: number
  let returnPct: number
  let maxDdPct: number
  let sharpe: number
  let winRate: number
  let totalFills: number

  if (props.precomputed) {
    totalPnl   = props.precomputed.totalPnl
    returnPct  = props.precomputed.returnPct
    maxDdPct   = props.precomputed.maxDdPct
    sharpe     = props.precomputed.sharpe
    winRate    = props.precomputed.winRate
    totalFills = props.precomputed.totalFills
  } else {
    totalPnl  = fills.length ? fills[fills.length - 1].equity - startingCapital : 0
    returnPct = startingCapital ? (totalPnl / startingCapital) * 100 : 0

    // Peak-to-trough drawdown on the equity curve
    let peak = startingCapital
    let maxDd = 0
    for (const f of fills) {
      if (f.equity > peak) peak = f.equity
      const dd = (peak - f.equity) / peak
      if (dd > maxDd) maxDd = dd
    }
    maxDdPct = maxDd * 100

    // Realised PnL per fill → simple Sharpe estimate
    const realised = fills.filter((f) => f.realizedPnl !== 0).map((f) => f.realizedPnl)
    const mean = realised.length ? realised.reduce((a, b) => a + b, 0) / realised.length : 0
    const variance = realised.length
      ? realised.reduce((a, b) => a + (b - mean) ** 2, 0) / realised.length
      : 0
    const std = Math.sqrt(variance)
    sharpe = std > 0 ? mean / std : 0

    // Win rate on the realised-PnL fills (the closing leg of each round trip)
    const wins = realised.filter((p) => p > 0).length
    winRate = realised.length ? (wins / realised.length) * 100 : 0
    totalFills = fills.length
  }

  const pnlColour =
    totalPnl > 0 ? 'stat-value--green' : totalPnl < 0 ? 'stat-value--red' : 'stat-value--muted'
  const retColour =
    returnPct > 0 ? 'stat-value--green' : returnPct < 0 ? 'stat-value--red' : 'stat-value--muted'

  return (
    <div className="panel">
      <div className="panel-header">
        <span className="panel-title">Risk</span>
      </div>
      <div className="stat-grid">
        <div className="stat-cell">
          <span className="stat-label">Total PnL</span>
          <span className={`stat-value ${pnlColour}`}>
            {totalPnl >= 0 ? '+' : ''}${totalPnl.toFixed(2)}
          </span>
        </div>
        <div className="stat-cell">
          <span className="stat-label">Return</span>
          <span className={`stat-value ${retColour}`}>
            {returnPct >= 0 ? '+' : ''}
            {returnPct.toFixed(2)}%
          </span>
        </div>
        <div className="stat-cell">
          <span className="stat-label">Max DD</span>
          <span className="stat-value stat-value--red stat-value--sm">
            -{maxDdPct.toFixed(2)}%
          </span>
        </div>
        <div className="stat-cell">
          <span className="stat-label">Sharpe</span>
          <span className="stat-value stat-value--sm">{sharpe.toFixed(2)}</span>
        </div>
        <div className="stat-cell">
          <span className="stat-label">Win Rate</span>
          <span className="stat-value stat-value--sm">{winRate.toFixed(1)}%</span>
        </div>
        <div className="stat-cell">
          <span className="stat-label">Fills</span>
          <span className="stat-value stat-value--sm stat-value--muted">{totalFills}</span>
        </div>
      </div>
    </div>
  )
}
