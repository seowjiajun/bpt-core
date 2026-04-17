import { useStore } from '../store'

const fmt = (v: number, d: number = 2) => v.toFixed(d)
const fmtBps = (v: number) => v.toFixed(1)
const fmtSci = (v: number) => v.toExponential(2)

function SuppressIndicator({ suppressed, reason }: { suppressed: boolean; reason: string }) {
  if (!suppressed) return <span className="stat-value--green">LIVE</span>
  const labels: Record<string, string> = {
    drift: 'DRIFT',
    tyr: 'TYR',
    inventory: 'INV',
    vol_gate: 'VHALT',
  }
  return <span className="stat-value--red">{labels[reason] ?? 'OFF'}</span>
}

function InventoryBar({ pct }: { pct: number }) {
  const clamped = Math.min(pct, 100)
  const cls = clamped > 90 ? 'limit-breach' : clamped > 70 ? 'limit-warn' : 'limit-ok'
  return (
    <div className="limit-bar-track" style={{ width: 80 }}>
      <div className={`limit-bar-fill ${cls}`} style={{ width: `${clamped}%` }} />
    </div>
  )
}

export function StrategyStatePanel() {
  const ss = useStore((s) => s.strategyState)

  if (!ss) {
    return (
      <div className="panel" style={{ gridArea: 'stratstate' }}>
        <div className="panel-header">
          <span className="panel-title">Strategy State</span>
          <span className="panel-badge">AS</span>
        </div>
        <div style={{ padding: '12px 16px', color: 'var(--text-muted)', fontSize: 13 }}>
          Waiting for strategy data.
        </div>
      </div>
    )
  }

  const driftDir = ss.drift > 0.0001 ? 'UP' : ss.drift < -0.0001 ? 'DN' : '--'
  const driftClass = ss.drift > 0.0001 ? 'stat-value--green' : ss.drift < -0.0001 ? 'stat-value--red' : 'stat-value--muted'
  const invSign = ss.inventory > 0 ? '+' : ''

  return (
    <div className="panel" style={{ gridArea: 'stratstate' }}>
      <div className="panel-header">
        <span className="panel-title">Strategy State</span>
        <span className="panel-badge">
          AS · {ss.warmedUp ? 'ACTIVE' : `WARMUP ${ss.volTicks}/${ss.volWarmup}`}
        </span>
      </div>

      <table className="blotter-table" style={{ fontSize: 11 }}>
        <thead>
          <tr>
            <th colSpan={2} style={{ textAlign: 'left' }}>Model</th>
            <th colSpan={2} style={{ textAlign: 'left' }}>State</th>
          </tr>
        </thead>
        <tbody>
          <tr>
            <td style={{ color: 'var(--text-muted)', width: 55 }}>DRIFT</td>
            <td className={`num ${driftClass}`}>{fmtBps(ss.driftBps)} bps {driftDir}</td>
            <td style={{ color: 'var(--text-muted)', width: 50 }}>BID</td>
            <td className="num"><SuppressIndicator suppressed={ss.bidSuppressed} reason={ss.bidSuppressReason} /></td>
          </tr>
          <tr>
            <td style={{ color: 'var(--text-muted)' }}>VOL</td>
            <td className="num">{fmtSci(ss.sigma2)} /s</td>
            <td style={{ color: 'var(--text-muted)' }}>ASK</td>
            <td className="num"><SuppressIndicator suppressed={ss.askSuppressed} reason={ss.askSuppressReason} /></td>
          </tr>
          <tr>
            <td style={{ color: 'var(--text-muted)' }}>REGIME</td>
            <td className={`num ${ss.regime === 'TRENDING' ? 'stat-value--red' : ss.regime === 'MEAN_REVERT' ? 'stat-value--green' : 'stat-value--muted'}`}>
              {ss.regime === 'WARMING_UP' ? 'WARM' : ss.regime === 'MEAN_REVERT' ? 'MR' : ss.regime === 'TRENDING' ? 'TREND' : 'NEUT'}
              {' '}H={fmt(ss.hurst, 2)}
            </td>
            <td style={{ color: 'var(--text-muted)' }}>VGATE</td>
            <td className="num">
              <span className={ss.volGateHalted ? 'stat-value--red' : 'stat-value--green'}>
                {ss.volGateHalted ? 'HALTED' : 'OK'}{ss.volGateTrips > 0 ? ` (${ss.volGateTrips})` : ''}
              </span>
            </td>
          </tr>
          <tr>
            <td style={{ color: 'var(--text-muted)' }}>GAMMA</td>
            <td className="num">
              {fmt(ss.gammaEffective, 2)}
              {ss.gammaMultiplier !== 1.0 && <span style={{ color: 'var(--text-muted)', fontSize: 9 }}> ({ss.gammaMultiplier.toFixed(1)}x)</span>}
            </td>
            <td style={{ color: 'var(--text-muted)' }}>INV</td>
            <td className="num">{invSign}{fmt(ss.inventory, 6)}</td>
          </tr>
          <tr>
            <td style={{ color: 'var(--text-muted)' }}>SPREAD</td>
            <td className="num">{fmtBps(ss.halfSpreadBps)} bps</td>
            <td style={{ color: 'var(--text-muted)' }}></td>
            <td className="num">
              <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'flex-end', gap: 6 }}>
                <InventoryBar pct={ss.inventoryPct} />
                <span className={ss.inventoryPct > 90 ? 'stat-value--red' : ss.inventoryPct > 70 ? 'limit-warn' : 'stat-value--muted'} style={{ fontSize: 10 }}>
                  {fmt(ss.inventoryPct, 0)}%
                </span>
              </div>
            </td>
          </tr>
          <tr>
            <td style={{ color: 'var(--text-muted)' }}>R-MID</td>
            <td className={`num ${ss.reservationOffsetBps > 0.5 ? 'stat-value--green' : ss.reservationOffsetBps < -0.5 ? 'stat-value--red' : 'stat-value--muted'}`}>
              {ss.reservationOffsetBps >= 0 ? '+' : ''}{fmtBps(ss.reservationOffsetBps)} bps
            </td>
            <td style={{ color: 'var(--text-muted)' }}>KAPPA</td>
            <td className="num">{fmt(ss.kappa, 3)}{ss.kappaLive ? '' : ' (fb)'}</td>
          </tr>
        </tbody>
      </table>
    </div>
  )
}
