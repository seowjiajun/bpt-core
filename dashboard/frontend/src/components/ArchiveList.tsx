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
  // Universal-core fields — optional so older runs (pre-2026-04-26) still load.
  buy_count?: number
  sell_count?: number
  buy_notional_usd?: number
  sell_notional_usd?: number
  simulation_start?: string
  simulation_end?: string
  wallclock_duration_ms?: number
  instruments?: string[]
  strategy_name?: string
  params_hash?: string
  git_sha?: string
}

type SortKey =
  | 'name'
  | 'strategy_name'
  | 'return_pct'
  | 'max_drawdown_pct'
  | 'sharpe_per_fill'
  | 'total_fills'
  | 'win_rate_pct'

interface Props {
  onOpen: (name: string) => void
  onCompare: (runA: string, runB: string) => void
  onSweep: (runs: string[]) => void
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

export function ArchiveList({ onOpen, onCompare, onSweep }: Props) {
  const [runs, setRuns] = useState<RunRow[] | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [sortKey, setSortKey] = useState<SortKey>('name')
  const [sortDesc, setSortDesc] = useState(true)
  // Compare-selection: at most two runs at a time. Order = pick order, so
  // the diff column "Δ (B − A)" reads "second pick − first pick".
  const [selected, setSelected] = useState<string[]>([])
  // Filter inputs — case-insensitive substring match on each field.
  // Empty filter = match-all. Useful once a sweep has produced 50+ runs
  // and the list scrolls.
  const [fStrategy, setFStrategy] = useState('')
  const [fParams, setFParams] = useState('')
  const [fInstrument, setFInstrument] = useState('')

  useEffect(() => {
    fetch('/api/backtest-runs')
      .then((r) => (r.ok ? r.json() : Promise.reject(new Error(`HTTP ${r.status}`))))
      .then((data: RunRow[]) => setRuns(data))
      .catch((e) => setError(String(e)))
  }, [])

  const sorted = useMemo(() => {
    if (!runs) return []
    const ql = fStrategy.trim().toLowerCase()
    const qp = fParams.trim().toLowerCase()
    const qi = fInstrument.trim().toLowerCase()
    const filtered = runs.filter((r) => {
      if (ql && !(r.strategy_name ?? '').toLowerCase().includes(ql)) return false
      if (qp && !(r.params_hash ?? '').toLowerCase().includes(qp)) return false
      if (qi) {
        const insts = (r.instruments ?? []).join(' ').toLowerCase()
        if (!insts.includes(qi)) return false
      }
      return true
    })
    filtered.sort((a, b) => {
      const av = a[sortKey]
      const bv = b[sortKey]
      if (typeof av === 'string' && typeof bv === 'string') {
        return sortDesc ? bv.localeCompare(av) : av.localeCompare(bv)
      }
      return sortDesc ? (bv as number) - (av as number) : (av as number) - (bv as number)
    })
    return filtered
  }, [runs, sortKey, sortDesc, fStrategy, fParams, fInstrument])

  const toggleSort = (k: SortKey) => {
    if (k === sortKey) setSortDesc((d) => !d)
    else {
      setSortKey(k)
      setSortDesc(true)
    }
  }

  const arrow = (k: SortKey) => (k === sortKey ? (sortDesc ? ' ↓' : ' ↑') : '')

  const toggleSelect = (name: string) => {
    setSelected((cur) => {
      if (cur.includes(name)) return cur.filter((n) => n !== name)
      // No cap — 2 picks → diff view, 3+ picks → sweep view. The
      // button labels in the header switch automatically.
      return [...cur, name]
    })
  }

  return (
    <div className="archive-body">
      <div className="panel" style={{ height: '100%' }}>
        <div className="panel-header">
          <span className="panel-title">Backtest Runs</span>
          <span
            className="panel-badge"
            style={{ flex: 1, textAlign: 'left', marginLeft: 16, color: 'var(--text-muted)' }}
          >
            {selected.length === 0 && 'tip: tick 2 to compare, 3+ to sweep'}
            {selected.length === 1 && `selected: 1 (pick one more to compare, or 2+ more to sweep)`}
            {selected.length >= 2 && `selected: ${selected.length}`}
          </span>
          {selected.length === 2 && (
            <button
              className="kill-switch"
              style={{
                marginRight: 12,
                background: 'rgba(56,139,253,0.18)',
                color: 'var(--blue)',
                borderColor: 'var(--blue)',
              }}
              onClick={() => onCompare(selected[0], selected[1])}
            >
              Compare →
            </button>
          )}
          {selected.length >= 3 && (
            <button
              className="kill-switch"
              style={{
                marginRight: 12,
                background: 'rgba(210,153,34,0.18)',
                color: 'var(--yellow)',
                borderColor: 'var(--yellow)',
              }}
              onClick={() => onSweep(selected)}
            >
              Sweep ({selected.length}) →
            </button>
          )}
          <span className="panel-badge">
            {runs ? `${runs.length} run${runs.length === 1 ? '' : 's'}` : '—'}
          </span>
        </div>
        <div className="panel-body panel-body--flush">
          {/* Filter strip — substring match on each field. Lives in the
              panel body so it scrolls with the table; sticky positioning
              keeps it visible regardless. */}
          <div className="archive-filter-row">
            <input
              className="archive-filter-input"
              type="text"
              placeholder="strategy"
              value={fStrategy}
              onChange={(e) => setFStrategy(e.target.value)}
            />
            <input
              className="archive-filter-input"
              type="text"
              placeholder="params hash prefix"
              value={fParams}
              onChange={(e) => setFParams(e.target.value)}
            />
            <input
              className="archive-filter-input"
              type="text"
              placeholder="instrument (e.g. APE)"
              value={fInstrument}
              onChange={(e) => setFInstrument(e.target.value)}
            />
            {(fStrategy || fParams || fInstrument) && (
              <button
                className="archive-filter-clear"
                onClick={() => {
                  setFStrategy('')
                  setFParams('')
                  setFInstrument('')
                }}
              >
                clear
              </button>
            )}
            <span className="archive-filter-count">
              {sorted.length} / {runs?.length ?? 0}
            </span>
          </div>
          {error && (
            <div style={{ padding: 16, color: 'var(--red)' }}>Error loading runs: {error}</div>
          )}
          {!error && runs === null && (
            <div style={{ padding: 16, color: 'var(--text-muted)' }}>Loading…</div>
          )}
          {!error && runs !== null && runs.length === 0 && (
            <div style={{ padding: 16, color: 'var(--text-muted)' }}>
              No runs found in bpt-backtester/results/.
            </div>
          )}
          {runs && runs.length > 0 && (
            <table className="blotter-table archive-table">
              <thead>
                <tr>
                  <th style={{ width: 28 }}></th>
                  <th onClick={() => toggleSort('strategy_name')} className="th-sortable">
                    Strategy{arrow('strategy_name')}
                  </th>
                  <th>SHA</th>
                  <th>Params</th>
                  <th>Window</th>
                  <th>Instruments</th>
                  <th onClick={() => toggleSort('return_pct')} className="th-sortable num">
                    Return{arrow('return_pct')}
                  </th>
                  <th onClick={() => toggleSort('max_drawdown_pct')} className="th-sortable num">
                    Max DD{arrow('max_drawdown_pct')}
                  </th>
                  <th onClick={() => toggleSort('sharpe_per_fill')} className="th-sortable num">
                    Sharpe/fill{arrow('sharpe_per_fill')}
                  </th>
                  <th onClick={() => toggleSort('win_rate_pct')} className="th-sortable num">
                    Win %{arrow('win_rate_pct')}
                  </th>
                  <th onClick={() => toggleSort('total_fills')} className="th-sortable num">
                    Fills{arrow('total_fills')}
                  </th>
                  <th className="num">Wallclock</th>
                  <th className="num">Final equity</th>
                </tr>
              </thead>
              <tbody>
                {sorted.map((r) => {
                  // Window: prefer the simulation_start/end strings, fall back to
                  // parsing the run name (older runs were keyed solely on dates).
                  const window =
                    r.simulation_start && r.simulation_end
                      ? `${r.simulation_start.slice(0, 10)} → ${r.simulation_end.slice(0, 10)}`
                      : r.name
                  const isSel = selected.includes(r.name)
                  return (
                    <tr
                      key={r.name}
                      className={`archive-row${isSel ? ' archive-row--selected' : ''}`}
                      onClick={() => onOpen(r.name)}
                    >
                      <td style={{ textAlign: 'center' }} onClick={(e) => e.stopPropagation()}>
                        <input
                          type="checkbox"
                          checked={isSel}
                          onChange={() => toggleSelect(r.name)}
                        />
                      </td>
                      <td>{r.strategy_name ?? '—'}</td>
                      <td className="mono-7" style={{ color: 'var(--text-secondary)' }}>
                        {r.git_sha ? r.git_sha.slice(0, 7) : '—'}
                      </td>
                      <td className="mono-7" style={{ color: 'var(--text-secondary)' }}>
                        {r.params_hash ? r.params_hash.slice(0, 8) : '—'}
                      </td>
                      <td style={{ color: 'var(--text-secondary)' }}>{window}</td>
                      <td style={{ color: 'var(--text-secondary)' }}>
                        {r.instruments && r.instruments.length > 0 ? r.instruments.join(', ') : '—'}
                      </td>
                      <td className={`num ${pnlClass(r.return_pct)}`}>{fmtPct(r.return_pct)}</td>
                      <td className="num pnl-neg">{r.max_drawdown_pct.toFixed(2)}%</td>
                      <td className="num">{fmtNum(r.sharpe_per_fill)}</td>
                      <td className="num">{r.win_rate_pct.toFixed(2)}%</td>
                      <td className="num">{r.total_fills.toLocaleString()}</td>
                      <td className="num" style={{ color: 'var(--text-secondary)' }}>
                        {r.wallclock_duration_ms !== undefined
                          ? `${(r.wallclock_duration_ms / 1000).toFixed(1)}s`
                          : '—'}
                      </td>
                      <td className="num">
                        $
                        {r.final_equity.toLocaleString('en-US', {
                          minimumFractionDigits: 2,
                          maximumFractionDigits: 2,
                        })}
                      </td>
                    </tr>
                  )
                })}
              </tbody>
            </table>
          )}
        </div>
      </div>
    </div>
  )
}
