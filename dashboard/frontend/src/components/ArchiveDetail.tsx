import { useEffect, useState } from 'react'
import { Blotter, type Fill } from './Blotter'
import { EquityChart } from './EquityChart'
import { RiskPanel } from './RiskPanel'

// Shape returned by the Vite dev-server plugin's detail endpoint
// (see vite.config.ts). Numeric fields are already parsed.
interface MarkoutHorizon {
  resolved_fills: number
  avg_bps: number
  avg_buy_bps: number
  avg_sell_bps: number
}

interface Summary {
  starting_capital: number
  final_equity: number
  total_pnl: number
  return_pct: number
  max_drawdown_pct: number
  sharpe_per_fill: number
  total_fills: number
  win_fills: number
  win_rate_pct: number
  // Universal-core (optional — older runs lack these).
  buy_count?: number
  sell_count?: number
  buy_notional_usd?: number
  sell_notional_usd?: number
  simulation_start?: string
  simulation_end?: string
  wallclock_duration_ms?: number
  instruments?: string[]
  markouts?: {
    '50ms'?: MarkoutHorizon
    '1s'?: MarkoutHorizon
    '5s'?: MarkoutHorizon
    '30s'?: MarkoutHorizon
  }
}

interface TradeRow {
  ts: number
  exchange: string
  symbol: string
  orderId: number
  clientOrderId: string
  side: 'BUY' | 'SELL'
  type: string
  qty: number
  price: number
  realizedPnl: number
  equity: number
}

interface RunDetail {
  name: string
  summary: Summary | null
  trades: TradeRow[]
  pnlCurve: Array<{ ts: number; equity: number }>
}

interface Props {
  name: string
}

export function ArchiveDetail({ name }: Props) {
  const [data, setData] = useState<RunDetail | null>(null)
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    setData(null)
    setError(null)
    fetch(`/api/backtest-runs/${encodeURIComponent(name)}`)
      .then((r) => (r.ok ? r.json() : Promise.reject(new Error(`HTTP ${r.status}`))))
      .then((d: RunDetail) => setData(d))
      .catch((e) => setError(String(e)))
  }, [name])

  if (error) return <div style={{ padding: 16, color: 'var(--red)' }}>Error: {error}</div>
  if (!data) return <div style={{ padding: 16, color: 'var(--text-muted)' }}>Loading {name}…</div>
  if (!data.summary) return <div style={{ padding: 16, color: 'var(--red)' }}>Missing summary.json</div>

  const { summary, trades, pnlCurve } = data

  // Turn trade rows into the Fill shape the Blotter renders.  We synthesize
  // `seq` from the array index — it's only used as a React key and the
  // archive data is immutable, so index is stable.
  const fills: Fill[] = trades.map((t, i) => ({
    seq: i,
    ts: t.ts,
    orderId: t.orderId,
    side: t.side,
    orderType: 'LIMIT',  // archive runs don't persist order type — default to LIMIT
    qty: t.qty,
    price: t.price,
    fee: 0,              // not persisted in archive CSVs today
    realizedPnl: t.realizedPnl,
    equity: t.equity,
  }))

  // The pnl curve's first row is (ts=0, equity=starting_capital) which the
  // time-series chart can't plot. Drop it.
  const equityPoints = pnlCurve.filter((p) => p.ts > 0)

  const precomputed = {
    totalPnl:   summary.total_pnl,
    returnPct:  summary.return_pct,
    maxDdPct:   summary.max_drawdown_pct,
    sharpe:     summary.sharpe_per_fill,
    winRate:    summary.win_rate_pct,
    totalFills: summary.total_fills,
    buyCount:   summary.buy_count,
    sellCount:  summary.sell_count,
    buyNotional:  summary.buy_notional_usd,
    sellNotional: summary.sell_notional_usd,
  }

  const metadataLine = [
    summary.instruments && summary.instruments.length > 0 ? summary.instruments.join(' · ') : null,
    summary.simulation_start && summary.simulation_end
      ? `${summary.simulation_start} → ${summary.simulation_end}`
      : null,
    summary.wallclock_duration_ms !== undefined
      ? `wallclock ${(summary.wallclock_duration_ms / 1000).toFixed(1)}s`
      : null,
  ]
    .filter((s): s is string => s !== null)
    .join('   ')

  const markouts = summary.markouts
  const hasMarkouts = !!markouts && Object.values(markouts).some((h) => h && h.resolved_fills > 0)
  const markoutHorizons: Array<['50ms' | '1s' | '5s' | '30s', MarkoutHorizon | undefined]> = [
    ['50ms', markouts?.['50ms']],
    ['1s',   markouts?.['1s']],
    ['5s',   markouts?.['5s']],
    ['30s',  markouts?.['30s']],
  ]
  const bpsCell = (v: number | undefined) => {
    if (v === undefined) return <span style={{ color: 'var(--text-muted)' }}>—</span>
    const cls = v > 0.5 ? 'pnl-pos' : v < -0.5 ? 'pnl-neg' : 'pnl-zero'
    return <span className={cls}>{v >= 0 ? '+' : ''}{v.toFixed(2)}</span>
  }

  return (
    <div className={`archive-body archive-body--detail${hasMarkouts ? ' archive-body--detail-markouts' : ''}`}>
      <div style={{ gridArea: 'risk', display: 'grid' }}>
        <RiskPanel
          fills={fills}
          startingCapital={summary.starting_capital}
          precomputed={precomputed}
        />
      </div>

      {hasMarkouts && (
        <div className="panel" style={{ gridArea: 'markouts' }}>
          <div className="panel-header">
            <span className="panel-title">Markouts (bps)</span>
            <span className="panel-badge" style={{ color: 'var(--text-muted)' }}>
              positive = price moved with you · 4 horizons
            </span>
          </div>
          <div style={{ padding: 12, overflow: 'auto' }}>
            <table className="blotter-table" style={{ width: '100%' }}>
              <thead>
                <tr>
                  <th>Horizon</th>
                  <th className="num">All</th>
                  <th className="num">Buys</th>
                  <th className="num">Sells</th>
                  <th className="num">Resolved</th>
                </tr>
              </thead>
              <tbody>
                {markoutHorizons.map(([label, h]) => (
                  <tr key={label}>
                    <td>{label}</td>
                    <td className="num">{bpsCell(h?.avg_bps)}</td>
                    <td className="num">{bpsCell(h?.avg_buy_bps)}</td>
                    <td className="num">{bpsCell(h?.avg_sell_bps)}</td>
                    <td className="num" style={{ color: 'var(--text-muted)' }}>{h?.resolved_fills ?? 0}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </div>
      )}

      <div className="panel" style={{ gridArea: 'equity' }}>
        <div className="panel-header">
          <span className="panel-title">Equity Curve</span>
          <span className="panel-badge" style={{ flex: 1, textAlign: 'left', marginLeft: 16, color: 'var(--text-muted)' }}>
            {metadataLine}
          </span>
          <span className="panel-badge">
            ${summary.final_equity.toLocaleString('en-US', { minimumFractionDigits: 2, maximumFractionDigits: 2 })}
            {' · '}
            {equityPoints.length.toLocaleString()} pts
          </span>
        </div>
        <EquityChart
          fills={equityPoints}
          startingCapital={summary.starting_capital}
          fillMarkers={fills}
        />
      </div>

      <Blotter fills={fills} />
    </div>
  )
}
