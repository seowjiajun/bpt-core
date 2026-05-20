import { useMemo, useState } from 'react'
import { useStore } from '../store'

export interface Fill {
  seq: number // client-side monotonic counter; used as React key so
  // partial fills (which share orderId) don't collide
  ts: number // ns since epoch
  orderId: number
  symbol: string // venue-native symbol from the FillMsg
  side: 'BUY' | 'SELL'
  orderType: string
  qty: number
  price: number
  fee: number
  realizedPnl: number
  equity: number
}

function fmtTime(ts: number) {
  return new Date(ts / 1_000_000).toISOString().slice(11, 23)
}

function fmtPrice(p: number) {
  return p.toLocaleString('en-US', { minimumFractionDigits: 1, maximumFractionDigits: 1 })
}

function pnlClass(pnl: number) {
  if (pnl > 0) return 'pnl-pos'
  if (pnl < 0) return 'pnl-neg'
  return 'pnl-zero'
}

// Cap rendered rows so the DOM doesn't bloat past human-readable size.
// At ~100 fills/session this is moot; if a session generates >500 fills the
// trader's looking at recent activity anyway — older fills stay in the
// store and can be inspected via /archive after the session.
const MAX_RENDERED_ROWS = 500

type SideFilter = 'ALL' | 'BUY' | 'SELL'

interface BlotterProps {
  // Optional override — when provided, render these fills instead of the
  // live store.  Used by the backtest archive view.
  fills?: Fill[]
}

export function Blotter(props: BlotterProps = {}) {
  const liveFills = useStore((s) => s.fills)
  const fills = props.fills ?? liveFills

  const [sideFilter, setSideFilter] = useState<SideFilter>('ALL')
  const [symbolFilter, setSymbolFilter] = useState<Set<string>>(new Set())

  // Distinct symbols across the current fills set — used to render the
  // symbol-pill row dynamically. useMemo so re-renders on unrelated state
  // changes don't re-scan the fills array.
  const allSymbols = useMemo(() => {
    const s = new Set<string>()
    for (const f of fills) s.add(f.symbol)
    return Array.from(s).sort()
  }, [fills])

  // Apply filters. Empty symbolFilter set = "no filter" (show all symbols).
  const filtered = useMemo(() => {
    return fills.filter((f) => {
      if (sideFilter !== 'ALL' && f.side !== sideFilter) return false
      if (symbolFilter.size > 0 && !symbolFilter.has(f.symbol)) return false
      return true
    })
  }, [fills, sideFilter, symbolFilter])

  // Compute per-order fill counts and index-within-order so the blotter
  // can show partial fills visually. Process chronologically (oldest first)
  // to get correct 1/N, 2/N labels, then reverse for newest-first display.
  // Counts are computed over the *filtered* set so the displayed N/N
  // labels match what's visible.
  const totalByOrder = new Map<number, number>()
  for (const f of filtered) {
    totalByOrder.set(f.orderId, (totalByOrder.get(f.orderId) ?? 0) + 1)
  }
  const seenByOrder = new Map<number, number>()
  const allRows = filtered.map((f) => {
    const total = totalByOrder.get(f.orderId) ?? 1
    const idx = (seenByOrder.get(f.orderId) ?? 0) + 1
    seenByOrder.set(f.orderId, idx)
    return { fill: f, idx, total }
  })
  allRows.reverse()
  // Cap rendered rows; older fills remain in the store for /archive replay.
  const rows = allRows.slice(0, MAX_RENDERED_ROWS)
  const truncated = allRows.length > MAX_RENDERED_ROWS

  const toggleSymbol = (sym: string) => {
    setSymbolFilter((prev) => {
      const next = new Set(prev)
      if (next.has(sym)) next.delete(sym)
      else next.add(sym)
      return next
    })
  }

  return (
    <div className="panel" style={{ gridArea: 'blotter' }}>
      <div className="panel-header">
        <span className="panel-title">Blotter</span>
        <span className="panel-badge">
          {filtered.length}
          {filtered.length !== fills.length ? ` / ${fills.length}` : ''} fills
        </span>
      </div>
      <div className="blotter-filters">
        {/* Side filter — radio-style trio */}
        <div className="blotter-filter-group">
          {(['ALL', 'BUY', 'SELL'] as SideFilter[]).map((s) => (
            <button
              key={s}
              type="button"
              className={`filter-pill${sideFilter === s ? ' filter-pill--active' : ''}`}
              onClick={() => setSideFilter(s)}
            >
              {s}
            </button>
          ))}
        </div>
        {/* Symbol filter — multi-select, hidden if only one symbol in the set */}
        {allSymbols.length > 1 && (
          <div className="blotter-filter-group">
            {allSymbols.map((sym) => (
              <button
                key={sym}
                type="button"
                className={`filter-pill${symbolFilter.has(sym) ? ' filter-pill--active' : ''}`}
                onClick={() => toggleSymbol(sym)}
                title={
                  symbolFilter.size === 0
                    ? `Click to filter to ${sym}`
                    : symbolFilter.has(sym)
                      ? `Click to unselect ${sym}`
                      : `Click to also show ${sym}`
                }
              >
                {sym}
              </button>
            ))}
          </div>
        )}
      </div>
      <div className="panel-body panel-body--flush">
        <table className="blotter-table">
          <thead>
            <tr>
              <th>Time (UTC)</th>
              <th>ID</th>
              <th>Symbol</th>
              <th>Side</th>
              <th>Type</th>
              <th>Qty</th>
              <th>Price</th>
              <th>Fee</th>
              <th>Realized PnL</th>
              <th>Equity</th>
            </tr>
          </thead>
          <tbody>
            {rows.length === 0 && (
              <tr>
                <td
                  colSpan={10}
                  style={{ textAlign: 'center', color: 'var(--text-muted)', padding: '20px' }}
                >
                  {fills.length === 0 ? 'No fills yet' : 'No fills match filters'}
                </td>
              </tr>
            )}
            {rows.map(({ fill: f, idx, total }) => {
              // Order ID display:
              //   single fill        →  "#<id>"
              //   first of N fills   →  "#<id> (1/N)"
              //   continuation fill  →  "↳ (n/N)"  — dim, continuation arrow
              const isPartial = total > 1
              const isContinuation = isPartial && idx > 1
              return (
                <tr key={f.seq}>
                  <td>{fmtTime(f.ts)}</td>
                  <td style={{ color: 'var(--text-muted)' }}>
                    {isContinuation ? (
                      <span style={{ opacity: 0.5 }}>
                        ↳ ({idx}/{total})
                      </span>
                    ) : isPartial ? (
                      <>
                        #{f.orderId} <span style={{ opacity: 0.6 }}>(1/{total})</span>
                      </>
                    ) : (
                      <>#{f.orderId}</>
                    )}
                  </td>
                  <td style={{ color: 'var(--text-muted)' }}>{f.symbol}</td>
                  <td className={f.side === 'BUY' ? 'side-buy' : 'side-sell'}>{f.side}</td>
                  <td style={{ color: 'var(--text-muted)' }}>{f.orderType}</td>
                  <td>{f.qty.toFixed(4)}</td>
                  <td>{fmtPrice(f.price)}</td>
                  <td style={{ color: 'var(--text-muted)' }}>
                    {f.fee === 0 ? '—' : `$${f.fee.toFixed(4)}`}
                  </td>
                  <td className={pnlClass(f.realizedPnl)}>
                    {f.realizedPnl === 0
                      ? '—'
                      : `${f.realizedPnl >= 0 ? '+' : ''}$${f.realizedPnl.toFixed(2)}`}
                  </td>
                  <td>
                    $
                    {f.equity.toLocaleString('en-US', {
                      minimumFractionDigits: 2,
                      maximumFractionDigits: 2,
                    })}
                  </td>
                </tr>
              )
            })}
            {truncated && (
              <tr>
                <td
                  colSpan={10}
                  style={{
                    textAlign: 'center',
                    color: 'var(--text-muted)',
                    padding: '8px',
                    fontSize: '11px',
                    fontStyle: 'italic',
                  }}
                >
                  Showing newest {MAX_RENDERED_ROWS} of {allRows.length} — older fills in store
                </td>
              </tr>
            )}
          </tbody>
        </table>
      </div>
    </div>
  )
}
