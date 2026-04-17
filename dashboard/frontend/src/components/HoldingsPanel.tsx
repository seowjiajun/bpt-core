import { useStore } from '../store'

// Holdings breakdown — table of open positions from heimdall AccountSnapshot.
//
// Distinct from PositionPanel: PositionPanel shows ONE instrument (the
// one the bridge's fill stream covers), derived client-side from fills.
// HoldingsPanel shows EVERYTHING the exchange thinks you hold, as
// reported by clearinghouseState / equivalent — including positions the
// current strategy didn't open and the USDC cash collateral. The
// canonical answer to "what am I actually carrying on this account".
export function HoldingsPanel() {
  const positions = useStore((s) => s.accountPositions)
  const balance = useStore((s) => s.accountBalance)
  const equity = useStore((s) => s.accountEquity)
  const exchange = useStore((s) => s.exchange)

  // Show panel whenever we have ANY account state: cash only, positions
  // only, or both. Previously this hid when positions.length === 0, which
  // meant a flat account looked like "no data" instead of "just cash".
  const hasData = positions.length > 0 || balance > 0 || equity > 0
  if (!hasData) {
    return (
      <div className="panel" style={{ gridArea: 'holdings' }}>
        <div className="panel-header">
          <span className="panel-title">Holdings</span>
          <span className="panel-badge">{exchange || '—'}</span>
        </div>
        <div style={{ padding: '12px 16px', color: 'var(--text-muted)', fontSize: 13 }}>
          Waiting for account snapshot.
        </div>
      </div>
    )
  }

  // Total dollarized exposure (sum of |notional|) and net uPnL across all legs.
  let totalGross = 0
  let totalPnl = 0
  const rows = positions.map((p) => {
    // Back out the mark price from entry + uPnL so we don't need a
    // separate per-symbol price feed: entry*qty + uPnL = mark*qty →
    // mark = (entry*qty + uPnL) / qty. Works for any nonzero qty.
    const mark = p.netQty !== 0 ? (p.avgEntry * p.netQty + p.unrealizedPnl) / p.netQty : 0
    const notional = Math.abs(p.netQty * mark)
    totalGross += notional
    totalPnl += p.unrealizedPnl
    return { ...p, mark, notional }
  })

  const pnlClass = (v: number) =>
    v > 0 ? 'stat-value--green' : v < 0 ? 'stat-value--red' : 'stat-value--muted'

  // Cash = withdrawable balance (what's actually sitting as USDC
  // collateral, not counting unrealized P&L on open positions).
  // Equity = balance + sum(unrealized) — shown in the Total row so the
  // user can see the reconciliation.
  return (
    <div className="panel" style={{ gridArea: 'holdings' }}>
      <div className="panel-header">
        <span className="panel-title">Holdings</span>
        <span className="panel-badge">{exchange || '—'}</span>
      </div>
      <table className="blotter-table">
        <thead>
          <tr>
            <th>Symbol</th>
            <th>Side</th>
            <th className="num">Qty</th>
            <th className="num">Avg Entry</th>
            <th className="num">Mark</th>
            <th className="num">Notional</th>
            <th className="num">uPnL</th>
          </tr>
        </thead>
        <tbody>
          {/* Cash row — always present so user can see USDC balance alongside crypto positions. */}
          <tr key="__usdc__">
            <td>USDC</td>
            <td className="stat-value--muted">CASH</td>
            <td className="num">
              {balance.toLocaleString('en-US', { minimumFractionDigits: 2, maximumFractionDigits: 2 })}
            </td>
            <td className="num">—</td>
            <td className="num">—</td>
            <td className="num">
              ${balance.toLocaleString('en-US', { minimumFractionDigits: 2, maximumFractionDigits: 2 })}
            </td>
            <td className="num stat-value--muted">—</td>
          </tr>
          {rows.map((r) => {
            const long = r.netQty > 0
            const sideClass = long ? 'side-buy' : 'side-sell'
            return (
              <tr key={r.symbol}>
                <td>{r.symbol}</td>
                <td className={sideClass}>{long ? 'LONG' : 'SHORT'}</td>
                <td className="num">{Math.abs(r.netQty).toFixed(4)}</td>
                <td className="num">
                  {r.avgEntry.toLocaleString('en-US', { minimumFractionDigits: 1, maximumFractionDigits: 1 })}
                </td>
                <td className="num">
                  {r.mark.toLocaleString('en-US', { minimumFractionDigits: 1, maximumFractionDigits: 1 })}
                </td>
                <td className="num">
                  ${r.notional.toLocaleString('en-US', { minimumFractionDigits: 2, maximumFractionDigits: 2 })}
                </td>
                <td className={`num ${pnlClass(r.unrealizedPnl)}`}>
                  {r.unrealizedPnl >= 0 ? '+' : ''}${r.unrealizedPnl.toFixed(2)}
                </td>
              </tr>
            )
          })}
          <tr style={{ borderTop: '1px solid var(--border)', fontWeight: 600 }}>
            <td colSpan={5}>Equity (cash + positions + uPnL)</td>
            <td className="num">
              ${equity.toLocaleString('en-US', { minimumFractionDigits: 2, maximumFractionDigits: 2 })}
            </td>
            <td className={`num ${pnlClass(totalPnl)}`}>
              {totalPnl >= 0 ? '+' : ''}${totalPnl.toFixed(2)}
            </td>
          </tr>
        </tbody>
      </table>
    </div>
  )
}
