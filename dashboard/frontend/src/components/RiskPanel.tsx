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
    // Optional per-side aggregates from summary.json (universal-core, may be
    // absent on pre-2026-04-26 runs).
    buyCount?: number
    sellCount?: number
    buyNotional?: number
    sellNotional?: number
    // Round-trip aggregates (closed open→flat→close cycles).
    closedRoundTrips?: number
    avgHoldingMs?: number
    medianHoldingMs?: number
    rtWinRatePct?: number
  }
}

export function RiskPanel(props: RiskPanelProps = {}) {
  // Two modes — see EquityChart for the same split:
  //   1. Live/paper — Total PnL / Return / Max DD come from the bpt-order-gateway
  //      AccountSnapshot history in the store. Sharpe / Win Rate / Fills
  //      still come from the local fill stream.
  //   2. Archive — props.fills + props.startingCapital + optional precomputed.
  const liveFills = useStore((s) => s.fills)
  const accountHistory = useStore((s) => s.accountHistory)
  const liveUnrealizedPnl = useStore((s) => s.unrealizedPnl)
  const isArchive = props.fills !== undefined
  const fills = props.fills ?? liveFills
  const startingCapital = props.startingCapital ?? 0

  let totalPnl: number
  let returnPct: number
  let maxDdPct: number
  let sharpe: number
  let winRate: number
  let totalFills: number

  if (props.precomputed) {
    totalPnl = props.precomputed.totalPnl
    returnPct = props.precomputed.returnPct
    maxDdPct = props.precomputed.maxDdPct
    sharpe = props.precomputed.sharpe
    winRate = props.precomputed.winRate
    totalFills = props.precomputed.totalFills
  } else if (isArchive) {
    totalPnl = fills.length ? fills[fills.length - 1].equity - startingCapital : 0
    returnPct = startingCapital ? (totalPnl / startingCapital) * 100 : 0

    let peak = startingCapital
    let maxDd = 0
    for (const f of fills) {
      if (f.equity > peak) peak = f.equity
      const dd = (peak - f.equity) / peak
      if (dd > maxDd) maxDd = dd
    }
    maxDdPct = maxDd * 100

    const realised = fills.filter((f) => f.realizedPnl !== 0).map((f) => f.realizedPnl)
    const mean = realised.length ? realised.reduce((a, b) => a + b, 0) / realised.length : 0
    const variance = realised.length
      ? realised.reduce((a, b) => a + (b - mean) ** 2, 0) / realised.length
      : 0
    sharpe = variance > 0 ? mean / Math.sqrt(variance) : 0

    const wins = realised.filter((p) => p > 0).length
    winRate = realised.length ? (wins / realised.length) * 100 : 0
    totalFills = fills.length
  } else {
    // Live mode — true trading PnL is realized (per fill) + unrealized
    // (current position mark). NOT equity-diff: on Hyperliquid (and any
    // venue with auto-collateralize between spot and perp), USDC moves
    // in and out of the perp accountValue without any trading happening,
    // so equity-diff treats deposits as profit and withdrawals as loss.
    // Both realizedPnl per fill and unrealizedPnl on current position
    // are deposit-immune by construction.
    const first = accountHistory[0]?.equity ?? 0
    const realisedSum = liveFills.reduce((acc, f) => acc + f.realizedPnl, 0)
    totalPnl = realisedSum + liveUnrealizedPnl
    returnPct = first ? (totalPnl / first) * 100 : 0

    // Max DD computed on cumulative realized PnL series (deposit-immune).
    // Approximation: drops intra-fill unrealized swings — drawdowns shorter
    // than fill cadence are invisible. Acceptable for AS-style frequencies
    // where we typically fill multiple times per minute. If fills are sparse,
    // revisit by building a true MTM equity series via fold of trade prices
    // over time.
    let runningPnl = 0
    let peakPnl = 0
    let maxDd = 0
    for (const f of liveFills) {
      runningPnl += f.realizedPnl
      if (runningPnl > peakPnl) peakPnl = runningPnl
      const dd = first > 0 ? (peakPnl - runningPnl) / first : 0
      if (dd > maxDd) maxDd = dd
    }
    maxDdPct = maxDd * 100

    const realised = liveFills.filter((f) => f.realizedPnl !== 0).map((f) => f.realizedPnl)
    const mean = realised.length ? realised.reduce((a, b) => a + b, 0) / realised.length : 0
    const variance = realised.length
      ? realised.reduce((a, b) => a + (b - mean) ** 2, 0) / realised.length
      : 0
    sharpe = variance > 0 ? mean / Math.sqrt(variance) : 0

    const wins = realised.filter((p) => p > 0).length
    winRate = realised.length ? (wins / realised.length) * 100 : 0
    totalFills = liveFills.length
  }

  const pnlColour =
    totalPnl > 0 ? 'stat-value--green' : totalPnl < 0 ? 'stat-value--red' : 'stat-value--muted'
  const retColour =
    returnPct > 0 ? 'stat-value--green' : returnPct < 0 ? 'stat-value--red' : 'stat-value--muted'

  // Per-side fill split is currently only carried through the archive
  // precomputed prop. In live mode we'd derive it from liveFills, but the
  // panel already covers Win Rate / Fills there, so skip rather than
  // double up.
  const buyCount = props.precomputed?.buyCount
  const sellCount = props.precomputed?.sellCount
  const buyNotional = props.precomputed?.buyNotional
  const sellNotional = props.precomputed?.sellNotional
  const showSideSplit = buyCount !== undefined && sellCount !== undefined

  const closedRTs = props.precomputed?.closedRoundTrips
  const avgHoldMs = props.precomputed?.avgHoldingMs
  const rtWinRate = props.precomputed?.rtWinRatePct
  const showRoundTrips = closedRTs !== undefined && closedRTs > 0
  const fmtHolding = (ms: number | undefined): string => {
    if (ms === undefined) return '—'
    if (ms < 1000) return `${ms.toFixed(0)}ms`
    if (ms < 60_000) return `${(ms / 1000).toFixed(1)}s`
    return `${(ms / 60_000).toFixed(1)}m`
  }

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
          <span className="stat-value stat-value--red stat-value--sm">-{maxDdPct.toFixed(2)}%</span>
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
        {showSideSplit && (
          <>
            <div className="stat-cell">
              <span className="stat-label">Buys</span>
              <span className="stat-value stat-value--sm">
                {buyCount}
                {buyNotional !== undefined && (
                  <span style={{ color: 'var(--text-muted)', fontSize: 11, marginLeft: 8 }}>
                    ${buyNotional.toFixed(0)}
                  </span>
                )}
              </span>
            </div>
            <div className="stat-cell">
              <span className="stat-label">Sells</span>
              <span className="stat-value stat-value--sm">
                {sellCount}
                {sellNotional !== undefined && (
                  <span style={{ color: 'var(--text-muted)', fontSize: 11, marginLeft: 8 }}>
                    ${sellNotional.toFixed(0)}
                  </span>
                )}
              </span>
            </div>
          </>
        )}
        {showRoundTrips && (
          <>
            <div className="stat-cell">
              <span className="stat-label">Round-trips</span>
              <span className="stat-value stat-value--sm">
                {closedRTs}
                {rtWinRate !== undefined && (
                  <span style={{ color: 'var(--text-muted)', fontSize: 11, marginLeft: 8 }}>
                    {rtWinRate.toFixed(0)}% wins
                  </span>
                )}
              </span>
            </div>
            <div className="stat-cell">
              <span className="stat-label">Avg holding</span>
              <span className="stat-value stat-value--sm">{fmtHolding(avgHoldMs)}</span>
            </div>
          </>
        )}
      </div>
    </div>
  )
}
