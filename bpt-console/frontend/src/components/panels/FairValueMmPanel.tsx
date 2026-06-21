import type { FVMMStrategyState } from '../../types/messages'

const fmt = (v: number, d: number = 2) => v.toFixed(d)
const fmtBps = (v: number) => v.toFixed(2)

function InventoryBar({ pct }: { pct: number }) {
  const clamped = Math.min(pct, 100)
  const cls = clamped > 90 ? 'limit-breach' : clamped > 70 ? 'limit-warn' : 'limit-ok'
  return (
    <div className="limit-bar-track" style={{ width: 80 }}>
      <div className={`limit-bar-fill ${cls}`} style={{ width: `${clamped}%` }} />
    </div>
  )
}

function OrderDot({ live }: { live: boolean }) {
  return <span className={live ? 'stat-value--green' : 'stat-value--muted'}>{live ? 'LIVE' : 'NONE'}</span>
}

export function FairValueMmPanel({ state: ss }: { state: FVMMStrategyState }) {
  const invSign = ss.inventory > 0 ? '+' : ''
  const rpnlClass = ss.realizedPnl > 0 ? 'stat-value--green' : ss.realizedPnl < 0 ? 'stat-value--red' : 'stat-value--muted'
  const statusBadge = ss.shuttingDown
    ? 'SHUTDOWN'
    : ss.refdataStale
      ? 'STALE'
      : !ss.warmedUp
        ? `WARMUP ${ss.volTicks}/${ss.volWarmup}`
        : 'ACTIVE'

  return (
    <div className="panel" style={{ gridArea: 'stratstate' }}>
      <div className="panel-header">
        <span className="panel-title">Strategy State</span>
        <span className={`panel-badge${ss.shuttingDown || ss.refdataStale ? ' panel-badge--warn' : ''}`}>
          FVMM · {statusBadge}
        </span>
      </div>

      <table className="blotter-table" style={{ fontSize: 11 }}>
        <thead>
          <tr>
            <th colSpan={2} style={{ textAlign: 'left' }}>Model</th>
            <th colSpan={2} style={{ textAlign: 'left' }}>Orders</th>
          </tr>
        </thead>
        <tbody>
          <tr>
            <td style={{ color: 'var(--text-muted)', width: 55 }}>VOL σ</td>
            <td className="num">{fmtBps(ss.sigmaBps)} bps</td>
            <td style={{ color: 'var(--text-muted)', width: 50 }}>BID</td>
            <td className="num">
              <OrderDot live={ss.bidOrderLive} />
              {ss.bidOrderLive && ss.bidPrice > 0 && (
                <span className="stat-value--muted" style={{ fontSize: 10 }}>
                  {' '}{ss.bidPrice.toFixed(4)}
                </span>
              )}
            </td>
          </tr>
          <tr>
            <td style={{ color: 'var(--text-muted)' }}>SPREAD</td>
            <td className="num">{fmtBps(ss.halfSpreadBps)} bps/side</td>
            <td style={{ color: 'var(--text-muted)' }}>ASK</td>
            <td className="num">
              <OrderDot live={ss.askOrderLive} />
              {ss.askOrderLive && ss.askPrice > 0 && (
                <span className="stat-value--muted" style={{ fontSize: 10 }}>
                  {' '}{ss.askPrice.toFixed(4)}
                </span>
              )}
            </td>
          </tr>
          <tr>
            <td style={{ color: 'var(--text-muted)' }}>INV</td>
            <td className="num">
              {invSign}{fmt(ss.inventory, 4)}
            </td>
            <td style={{ color: 'var(--text-muted)' }}></td>
            <td className="num">
              <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'flex-end', gap: 6 }}>
                <InventoryBar pct={ss.inventoryPct} />
                <span
                  className={
                    ss.inventoryPct > 90
                      ? 'stat-value--red'
                      : ss.inventoryPct > 70
                        ? 'limit-warn'
                        : 'stat-value--muted'
                  }
                  style={{ fontSize: 10 }}
                >
                  {fmt(ss.inventoryPct, 0)}%
                </span>
              </div>
            </td>
          </tr>
          <tr>
            <td style={{ color: 'var(--text-muted)' }}>rPnL</td>
            <td className={`num ${rpnlClass}`}>
              {ss.realizedPnl >= 0 ? '+' : ''}{fmt(ss.realizedPnl, 4)} USD
            </td>
            <td style={{ color: 'var(--text-muted)' }}>MAX INV</td>
            <td className="num stat-value--muted">{fmt(ss.maxInventory, 2)}</td>
          </tr>
        </tbody>
      </table>
    </div>
  )
}
