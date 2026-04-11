import { useEffect, useMemo, useState } from 'react'

// Matches the shape returned by the Vite dev-server plugin in vite.config.ts.
// Whenever we graduate the endpoint into the bridge this type should move
// into src/types/ alongside the WS message types.
interface RunRow {
  name: string
  mtime: number
  starting_capital: number
  final_equity: number
  total_pnl: number
  return_pct: number
  max_drawdown_pct: number
  sharpe_per_fill: number
  total_fills: number
  win_rate_pct: number
}

type SortKey = 'name' | 'return_pct' | 'max_drawdown_pct' | 'sharpe_per_fill' | 'total_fills' | 'win_rate_pct'

interface Props {
  onOpen: (name: string) => void
}

function fmtPct(x: number) {
  return `${x >= 0 ? '+' : ''}${x.toFixed(2)}%`
}
function fmtNum(x: number, d = 4) {
  return x.toFixed(d)
}
function pnlClass(x: number) {
  if (x > 0) return 'pnl-pos'
  if (x < 0) return 'pnl-neg'
  return 'pnl-zero'
}

export function ArchiveList({ onOpen }: Props) {
  const [runs, setRuns] = useState<RunRow[] | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [sortKey, setSortKey] = useState<SortKey>('name')
  const [sortDesc, setSortDesc] = useState(true)

  useEffect(() => {
    fetch('/api/backtest-runs')
      .then((r) => (r.ok ? r.json() : Promise.reject(new Error(`HTTP ${r.status}`))))
      .then((data: RunRow[]) => setRuns(data))
      .catch((e) => setError(String(e)))
  }, [])

  const sorted = useMemo(() => {
    if (!runs) return []
    const copy = [...runs]
    copy.sort((a, b) => {
      const av = a[sortKey]
      const bv = b[sortKey]
      if (typeof av === 'string' && typeof bv === 'string') {
        return sortDesc ? bv.localeCompare(av) : av.localeCompare(bv)
      }
      return sortDesc ? (bv as number) - (av as number) : (av as number) - (bv as number)
    })
    return copy
  }, [runs, sortKey, sortDesc])

  const toggleSort = (k: SortKey) => {
    if (k === sortKey) setSortDesc((d) => !d)
    else {
      setSortKey(k)
      setSortDesc(true)
    }
  }

  const arrow = (k: SortKey) => (k === sortKey ? (sortDesc ? ' ↓' : ' ↑') : '')

  return (
    <div className="archive-body">
      <div className="panel" style={{ height: '100%' }}>
        <div className="panel-header">
          <span className="panel-title">Backtest Runs</span>
          <span className="panel-badge">{runs ? `${runs.length} run${runs.length === 1 ? '' : 's'}` : '—'}</span>
        </div>
        <div className="panel-body panel-body--flush">
          {error && <div style={{ padding: 16, color: 'var(--red)' }}>Error loading runs: {error}</div>}
          {!error && runs === null && <div style={{ padding: 16, color: 'var(--text-muted)' }}>Loading…</div>}
          {!error && runs !== null && runs.length === 0 && (
            <div style={{ padding: 16, color: 'var(--text-muted)' }}>
              No runs found in jormungandr/results/.
            </div>
          )}
          {runs && runs.length > 0 && (
            <table className="blotter-table archive-table">
              <thead>
                <tr>
                  <th onClick={() => toggleSort('name')} className="th-sortable">Run{arrow('name')}</th>
                  <th onClick={() => toggleSort('return_pct')} className="th-sortable num">Return{arrow('return_pct')}</th>
                  <th onClick={() => toggleSort('max_drawdown_pct')} className="th-sortable num">Max DD{arrow('max_drawdown_pct')}</th>
                  <th onClick={() => toggleSort('sharpe_per_fill')} className="th-sortable num">Sharpe/fill{arrow('sharpe_per_fill')}</th>
                  <th onClick={() => toggleSort('win_rate_pct')} className="th-sortable num">Win %{arrow('win_rate_pct')}</th>
                  <th onClick={() => toggleSort('total_fills')} className="th-sortable num">Fills{arrow('total_fills')}</th>
                  <th className="num">Final equity</th>
                </tr>
              </thead>
              <tbody>
                {sorted.map((r) => (
                  <tr key={r.name} onClick={() => onOpen(r.name)} className="archive-row">
                    <td>{r.name}</td>
                    <td className={`num ${pnlClass(r.return_pct)}`}>{fmtPct(r.return_pct)}</td>
                    <td className="num pnl-neg">{r.max_drawdown_pct.toFixed(2)}%</td>
                    <td className="num">{fmtNum(r.sharpe_per_fill)}</td>
                    <td className="num">{r.win_rate_pct.toFixed(2)}%</td>
                    <td className="num">{r.total_fills.toLocaleString()}</td>
                    <td className="num">${r.final_equity.toLocaleString('en-US', { minimumFractionDigits: 2, maximumFractionDigits: 2 })}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          )}
        </div>
      </div>
    </div>
  )
}
