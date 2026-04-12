import { useStore } from '../store'

export interface Fill {
  seq: number         // client-side monotonic counter; used as React key so
                      // partial fills (which share orderId) don't collide
  ts: number          // ns since epoch
  orderId: number
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

interface BlotterProps {
  // Optional override — when provided, render these fills instead of the
  // live store.  Used by the backtest archive view.
  fills?: Fill[]
}

export function Blotter(props: BlotterProps = {}) {
  const liveFills = useStore((s) => s.fills)
  const fills = props.fills ?? liveFills

  // Compute per-order fill counts and index-within-order so the blotter
  // can show partial fills visually. Process chronologically (oldest first)
  // to get correct 1/N, 2/N labels, then reverse for newest-first display.
  const totalByOrder = new Map<number, number>()
  for (const f of fills) {
    totalByOrder.set(f.orderId, (totalByOrder.get(f.orderId) ?? 0) + 1)
  }
  const seenByOrder = new Map<number, number>()
  const rows = fills.map((f) => {
    const total = totalByOrder.get(f.orderId) ?? 1
    const idx = (seenByOrder.get(f.orderId) ?? 0) + 1
    seenByOrder.set(f.orderId, idx)
    return { fill: f, idx, total }
  })
  rows.reverse()

  return (
    <div className="panel" style={{ gridArea: 'blotter' }}>
      <div className="panel-header">
        <span className="panel-title">Blotter</span>
        <span className="panel-badge">{fills.length} fills</span>
      </div>
      <div className="panel-body panel-body--flush">
        <table className="blotter-table">
          <thead>
            <tr>
              <th>Time (UTC)</th>
              <th>ID</th>
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
                <td colSpan={9} style={{ textAlign: 'center', color: 'var(--text-muted)', padding: '20px' }}>
                  No fills yet
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
                      <span style={{ opacity: 0.5 }}>↳ ({idx}/{total})</span>
                    ) : isPartial ? (
                      <>#{f.orderId} <span style={{ opacity: 0.6 }}>(1/{total})</span></>
                    ) : (
                      <>#{f.orderId}</>
                    )}
                  </td>
                  <td className={f.side === 'BUY' ? 'side-buy' : 'side-sell'}>{f.side}</td>
                  <td style={{ color: 'var(--text-muted)' }}>{f.orderType}</td>
                  <td>{f.qty.toFixed(4)}</td>
                  <td>{fmtPrice(f.price)}</td>
                  <td style={{ color: 'var(--text-muted)' }}>
                    {f.fee === 0 ? '—' : `$${f.fee.toFixed(4)}`}
                  </td>
                  <td className={pnlClass(f.realizedPnl)}>
                    {f.realizedPnl === 0 ? '—' : `${f.realizedPnl >= 0 ? '+' : ''}$${f.realizedPnl.toFixed(2)}`}
                  </td>
                  <td>${f.equity.toLocaleString('en-US', { minimumFractionDigits: 2, maximumFractionDigits: 2 })}</td>
                </tr>
              )
            })}
          </tbody>
        </table>
      </div>
    </div>
  )
}
