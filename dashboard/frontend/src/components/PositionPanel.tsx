import { useStore } from '../store'

export function PositionPanel() {
  const symbol = useStore((s) => s.symbol)
  const netQty = useStore((s) => s.netQty)
  const avgEntry = useStore((s) => s.avgEntry)
  const unrealizedPnl = useStore((s) => s.unrealizedPnl)
  const price = useStore((s) => s.price)

  const flat = netQty === 0
  const long = netQty > 0
  const side = flat ? 'FLAT' : long ? 'LONG' : 'SHORT'
  const sideColour = flat ? 'stat-value--muted' : long ? 'stat-value--green' : 'stat-value--red'
  const pnlColour =
    unrealizedPnl > 0
      ? 'stat-value--green'
      : unrealizedPnl < 0
        ? 'stat-value--red'
        : 'stat-value--muted'

  return (
    <div className="panel">
      <div className="panel-header">
        <span className="panel-title">Position</span>
        <span className="panel-badge">{symbol || '—'}</span>
      </div>
      <div className="stat-grid">
        <div className="stat-cell">
          <span className="stat-label">Side</span>
          <span className={`stat-value ${sideColour}`}>{side}</span>
        </div>
        <div className="stat-cell">
          <span className="stat-label">Qty</span>
          <span className="stat-value">{Math.abs(netQty).toFixed(4)}</span>
        </div>
        <div className="stat-cell">
          <span className="stat-label">Avg Entry</span>
          <span className="stat-value stat-value--sm">
            {flat ? '—' : avgEntry.toLocaleString('en-US', { minimumFractionDigits: 1 })}
          </span>
        </div>
        <div className="stat-cell">
          <span className="stat-label">Mark Price</span>
          <span className="stat-value stat-value--sm">
            {price ? price.toLocaleString('en-US', { minimumFractionDigits: 1 }) : '—'}
          </span>
        </div>
        <div className="stat-cell" style={{ gridColumn: '1 / -1' }}>
          <span className="stat-label">Unrealized PnL</span>
          <span className={`stat-value ${pnlColour}`}>
            {unrealizedPnl >= 0 ? '+' : ''}${unrealizedPnl.toFixed(2)}
          </span>
        </div>
      </div>
    </div>
  )
}
