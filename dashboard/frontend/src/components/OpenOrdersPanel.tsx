import { useStore, type OpenOrder } from '../store'

function fmtTime(ts: number) {
  return new Date(ts / 1_000_000).toISOString().slice(11, 23)
}

function fmtPrice(p: number) {
  return p.toLocaleString('en-US', { minimumFractionDigits: 1, maximumFractionDigits: 1 })
}

function statusClass(s: OpenOrder['status']): string {
  switch (s) {
    case 'acked':
      return 'order-status--acked'
    case 'partial':
      return 'order-status--partial'
    default:
      return ''
  }
}

export function OpenOrdersPanel() {
  const openOrders = useStore((s) => s.openOrders)
  const orders = [...openOrders.values()].sort((a, b) => b.ts - a.ts)

  return (
    <div className="panel" style={{ gridArea: 'orders' }}>
      <div className="panel-header">
        <span className="panel-title">Open Orders</span>
        <span className="panel-badge">{orders.length} working</span>
      </div>
      <div className="panel-body panel-body--flush">
        <table className="blotter-table">
          <thead>
            <tr>
              <th>Time</th>
              <th>ID</th>
              <th>Side</th>
              <th>Type</th>
              <th>Price</th>
              <th>Qty</th>
              <th>Filled</th>
              <th>Remaining</th>
              <th>Status</th>
            </tr>
          </thead>
          <tbody>
            {orders.length === 0 && (
              <tr>
                <td
                  colSpan={9}
                  style={{ textAlign: 'center', color: 'var(--text-muted)', padding: '20px' }}
                >
                  No open orders
                </td>
              </tr>
            )}
            {orders.map((o) => (
              <tr key={o.orderId}>
                <td>{fmtTime(o.ts)}</td>
                <td style={{ color: 'var(--text-muted)' }}>#{o.orderId}</td>
                <td className={o.side === 'BUY' ? 'side-buy' : 'side-sell'}>{o.side}</td>
                <td style={{ color: 'var(--text-muted)' }}>{o.orderType}</td>
                <td>{fmtPrice(o.price)}</td>
                <td>{o.qty.toFixed(4)}</td>
                <td>{o.filledQty.toFixed(4)}</td>
                <td>{o.remainingQty.toFixed(4)}</td>
                <td className={statusClass(o.status)}>{o.status.toUpperCase()}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  )
}
