// Perp Pulse — renders bpt-radar's perp-section MarketColor snapshot.
//
// Basis is (mark - index) / index × 1e4. Positive = contango / longs paying
// up to be long via perp; negative = backwardation / shorts paying. Magnitude
// scales with leverage demand and funding pressure between rate ticks.
//
// Sits as a sibling to OptionsPulsePanel + CalendarPanel under /radar. Each
// future radar section (flow color, vol regime) follows the same shape:
// pure consumer of useStore().marketColor[underlying].perp/flow/regime.

import { useStore } from '../store'
import type { MarketColorMsg } from '../types/messages'

type Num = number | null | undefined
const nullish = (v: Num): v is null | undefined => v == null || isNaN(v as number)

const fmtPx = (v: Num) =>
  nullish(v) ? '—' : (v as number).toLocaleString(undefined, { maximumFractionDigits: 2 })

// Basis sign carries the regime signal. Color escalates with magnitude so the
// operator can eyeball squeeze conditions: > 30bps is meaningfully stretched
// on majors, > 100bps is a heavy lean that often mean-reverts.
function basisClass(v: Num): string {
  if (nullish(v)) return 'stat-value--muted'
  const n = v as number
  if (Math.abs(n) < 1) return 'stat-value--muted'
  return n > 0 ? 'stat-value--green' : 'stat-value--red'
}

function basisLabel(v: Num): string {
  if (nullish(v)) return ''
  const n = v as number
  if (Math.abs(n) < 1) return 'flat'
  return n > 0 ? 'contango · longs paying' : 'backwardation · shorts paying'
}

function fmtBasis(v: Num): string {
  if (nullish(v)) return '—'
  const n = v as number
  const sign = n >= 0 ? '+' : ''
  return `${sign}${n.toFixed(2)} bps`
}

export function PerpPulsePanel({ underlying }: { underlying?: string | null } = {}) {
  const marketColor = useStore((s) => s.marketColor)

  const entry: [string, MarketColorMsg] | undefined = marketColor
    ? underlying && marketColor[underlying]
      ? [underlying, marketColor[underlying]]
      : (Object.entries(marketColor).sort(([a], [b]) => a.localeCompare(b))[0] as
          | [string, MarketColorMsg]
          | undefined)
    : undefined

  if (!entry) {
    return (
      <div className="panel">
        <div className="panel-header">
          <span className="panel-title">Perp Pulse</span>
          <span className="panel-badge">RADAR</span>
        </div>
        <div style={{ padding: '12px 16px', color: 'var(--text-muted)', fontSize: 13 }}>
          Waiting for radar data.
        </div>
      </div>
    )
  }

  const [u, msg] = entry
  const p = msg.perp
  const hasPerp = p && (p.basisBps != null || p.markPrice != null || p.indexPrice != null)

  return (
    <div className="panel">
      <div className="panel-header">
        <span className="panel-title">Perp Pulse · {u}</span>
        <span className="panel-badge">{msg.exchange}</span>
      </div>
      <div style={{ padding: '12px 16px', display: 'grid', gap: 12 }}>
        {!hasPerp && (
          <div style={{ color: 'var(--text-muted)', fontSize: 12 }}>
            No perp joined to this underlying yet — waiting for md-gateway InstrumentStats.
          </div>
        )}

        {hasPerp && (
          <>
            <div>
              <div
                style={{
                  color: 'var(--text-muted)',
                  fontSize: 11,
                  textTransform: 'uppercase',
                  letterSpacing: 1,
                }}
              >
                Perp basis vs index
              </div>
              <div style={{ display: 'flex', alignItems: 'baseline', gap: 12, marginTop: 4 }}>
                <span style={{ fontSize: 22, fontWeight: 700 }} className={basisClass(p.basisBps)}>
                  {fmtBasis(p.basisBps)}
                </span>
                <span style={{ color: 'var(--text-muted)', fontSize: 12 }}>{basisLabel(p.basisBps)}</span>
              </div>
            </div>

            <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 12, fontSize: 11 }}>
              <div>
                <div style={{ color: 'var(--text-muted)' }}>Mark</div>
                <div className="stat-value" style={{ fontSize: 14, fontWeight: 600 }}>
                  {fmtPx(p.markPrice)}
                </div>
              </div>
              <div>
                <div style={{ color: 'var(--text-muted)' }}>Index</div>
                <div className="stat-value" style={{ fontSize: 14, fontWeight: 600 }}>
                  {fmtPx(p.indexPrice)}
                </div>
              </div>
            </div>
          </>
        )}
      </div>
    </div>
  )
}
