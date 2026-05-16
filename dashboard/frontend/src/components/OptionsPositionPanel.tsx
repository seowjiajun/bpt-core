import { useMemo, useState } from 'react'
import type { OptionLeg } from '../types/options'

interface Props {
  legs: OptionLeg[]
}

type SortKey = 'expiry' | 'strike' | 'delta' | 'unrealizedPnl' | 'qty'

function fmtExpiry(d: number): string {
  const s = String(d)
  const y = s.slice(0, 4),
    m = s.slice(4, 6),
    day = s.slice(6, 8)
  return `${day}/${m}/${y.slice(2)}`
}

function fmtPrice(p: number): string {
  return p.toLocaleString('en-US', { minimumFractionDigits: 0, maximumFractionDigits: 0 })
}

function pnlClass(v: number): string {
  if (v > 0) return 'pnl-pos'
  if (v < 0) return 'pnl-neg'
  return 'pnl-zero'
}

export function OptionsPositionPanel({ legs }: Props) {
  const [sortKey, setSortKey] = useState<SortKey>('expiry')
  const [sortDesc, setSortDesc] = useState(false)

  const sorted = useMemo(() => {
    const copy = [...legs]
    copy.sort((a, b) => {
      const av = a[sortKey]
      const bv = b[sortKey]
      return sortDesc ? (bv as number) - (av as number) : (av as number) - (bv as number)
    })
    return copy
  }, [legs, sortKey, sortDesc])

  const toggleSort = (k: SortKey) => {
    if (k === sortKey) setSortDesc((d) => !d)
    else {
      setSortKey(k)
      setSortDesc(false)
    }
  }
  const arrow = (k: SortKey) => (k === sortKey ? (sortDesc ? ' ↓' : ' ↑') : '')

  // Aggregated row
  const agg = useMemo(() => {
    let delta = 0,
      gamma = 0,
      vega = 0,
      theta = 0,
      pnl = 0
    for (const l of legs) {
      const q = Math.abs(l.qty)
      delta += l.delta * q
      gamma += l.gamma * q
      vega += l.vega * q
      theta += l.theta * q
      pnl += l.unrealizedPnl
    }
    return { delta, gamma, vega, theta, pnl }
  }, [legs])

  return (
    <div className="panel">
      <div className="panel-header">
        <span className="panel-title">Options Positions</span>
        <span className="panel-badge">{legs.length} legs</span>
      </div>
      <div className="panel-body panel-body--flush">
        <table className="blotter-table">
          <thead>
            <tr>
              <th>UL</th>
              <th onClick={() => toggleSort('expiry')} className="th-sortable">
                Expiry{arrow('expiry')}
              </th>
              <th onClick={() => toggleSort('strike')} className="th-sortable num">
                Strike{arrow('strike')}
              </th>
              <th>Type</th>
              <th onClick={() => toggleSort('qty')} className="th-sortable num">
                Qty{arrow('qty')}
              </th>
              <th className="num">Entry</th>
              <th className="num">Mark</th>
              <th className="num">IV</th>
              <th onClick={() => toggleSort('delta')} className="th-sortable num">
                Δ{arrow('delta')}
              </th>
              <th className="num">Γ</th>
              <th className="num">V</th>
              <th className="num">Θ</th>
              <th onClick={() => toggleSort('unrealizedPnl')} className="th-sortable num">
                PnL{arrow('unrealizedPnl')}
              </th>
            </tr>
          </thead>
          <tbody>
            {sorted.map((l) => (
              <tr key={l.instrumentId}>
                <td style={{ fontWeight: 600 }}>{l.underlying}</td>
                <td>{fmtExpiry(l.expiry)}</td>
                <td className="num">{fmtPrice(l.strike)}</td>
                <td className={l.optionSide === 'CALL' ? 'side-buy' : 'side-sell'}>
                  {l.optionSide}
                </td>
                <td className={`num ${l.qty > 0 ? 'pnl-pos' : 'pnl-neg'}`}>{l.qty}</td>
                <td className="num">{fmtPrice(l.avgEntry)}</td>
                <td className="num">{fmtPrice(l.markPrice)}</td>
                <td className="num">{(l.iv * 100).toFixed(1)}%</td>
                <td className={`num ${pnlClass(l.delta)}`}>{l.delta.toFixed(3)}</td>
                <td className="num" style={{ color: 'var(--text-muted)' }}>
                  {l.gamma.toFixed(6)}
                </td>
                <td className="num" style={{ color: 'var(--text-muted)' }}>
                  {l.vega.toFixed(1)}
                </td>
                <td className={`num ${pnlClass(l.theta)}`}>{l.theta.toFixed(1)}</td>
                <td className={`num ${pnlClass(l.unrealizedPnl)}`}>
                  {l.unrealizedPnl >= 0 ? '+' : ''}${l.unrealizedPnl.toFixed(0)}
                </td>
              </tr>
            ))}
            <tr className="options-agg-row">
              <td colSpan={8} style={{ textAlign: 'right', fontWeight: 600 }}>
                Portfolio (mixed UL — Δ/Γ/V are per-underlying, sum is informational only)
              </td>
              <td className={`num ${pnlClass(agg.delta)}`} style={{ fontWeight: 600 }}>
                {agg.delta.toFixed(3)}
              </td>
              <td className="num" style={{ color: 'var(--text-muted)', fontWeight: 600 }}>
                {agg.gamma.toFixed(6)}
              </td>
              <td className="num" style={{ color: 'var(--text-muted)', fontWeight: 600 }}>
                {agg.vega.toFixed(1)}
              </td>
              <td className={`num ${pnlClass(agg.theta)}`} style={{ fontWeight: 600 }}>
                {agg.theta.toFixed(1)}
              </td>
              <td className={`num ${pnlClass(agg.pnl)}`} style={{ fontWeight: 600 }}>
                {agg.pnl >= 0 ? '+' : ''}${agg.pnl.toFixed(0)}
              </td>
            </tr>
          </tbody>
        </table>
      </div>
    </div>
  )
}
