import { useStore } from '../store'

export interface Fill {
  seq: number         // client-side monotonic counter; used as React key so
                      // partial fills (which share orderId) don't collide
  ts: number          // ns since epoch
  orderId: number
  side: 'BUY' | 'SELL'
  qty: number
  price: number
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
  // Newest fill first
  const rows = [...fills].reverse()

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
              <th>Qty</th>
              <th>Price</th>
              <th>Realized PnL</th>
              <th>Equity</th>
            </tr>
          </thead>
          <tbody>
            {rows.length === 0 && (
              <tr>
                <td colSpan={7} style={{ textAlign: 'center', color: 'var(--text-muted)', padding: '20px' }}>
                  No fills yet
                </td>
              </tr>
            )}
            {rows.map(f => (
              <tr key={f.seq}>
                <td>{fmtTime(f.ts)}</td>
                <td style={{ color: 'var(--text-muted)' }}>#{f.orderId}</td>
                <td className={f.side === 'BUY' ? 'side-buy' : 'side-sell'}>{f.side}</td>
                <td>{f.qty.toFixed(4)}</td>
                <td>{fmtPrice(f.price)}</td>
                <td className={pnlClass(f.realizedPnl)}>
                  {f.realizedPnl === 0 ? '—' : `${f.realizedPnl >= 0 ? '+' : ''}$${f.realizedPnl.toFixed(2)}`}
                </td>
                <td>${f.equity.toLocaleString('en-US', { minimumFractionDigits: 2, maximumFractionDigits: 2 })}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  )
}
