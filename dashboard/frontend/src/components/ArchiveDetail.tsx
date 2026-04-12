import { useEffect, useState } from 'react'
import { Blotter, type Fill } from './Blotter'
import { EquityChart } from './EquityChart'
import { RiskPanel } from './RiskPanel'

// Shape returned by the Vite dev-server plugin's detail endpoint
// (see vite.config.ts). Numeric fields are already parsed.
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
  }

  return (
    <div className="archive-body archive-body--detail">
      <div style={{ gridArea: 'risk', display: 'grid' }}>
        <RiskPanel
          fills={fills}
          startingCapital={summary.starting_capital}
          precomputed={precomputed}
        />
      </div>

      <div className="panel" style={{ gridArea: 'equity' }}>
        <div className="panel-header">
          <span className="panel-title">Equity Curve</span>
          <span className="panel-badge">
            ${summary.final_equity.toLocaleString('en-US', { minimumFractionDigits: 2, maximumFractionDigits: 2 })}
            {' · '}
            {equityPoints.length.toLocaleString()} pts
          </span>
        </div>
        <EquityChart fills={equityPoints} startingCapital={summary.starting_capital} />
      </div>

      <Blotter fills={fills} />
    </div>
  )
}
