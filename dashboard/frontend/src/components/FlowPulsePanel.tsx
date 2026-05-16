// Flow Pulse — renders bpt-radar's flow-section MarketColor snapshot.
//
// Aggressor flow on the underlying's perp over a 5-min rolling window.
// Positive imbalance = net aggressor lifting offers (paying up to be long /
// flat shorts), negative = net hitting bids. Notional is in quote ccy
// (USD-equivalent for crypto perps).
//
// Sibling to OptionsPulsePanel + PerpPulsePanel + CalendarPanel under /radar.

import { useStore } from '../store'
import type { MarketColorMsg } from '../types/messages'

type Num = number | null | undefined
const nullish = (v: Num): v is null | undefined => v == null || isNaN(v as number)

// Notional in $M when ≥ 1M; otherwise $k for sub-million. Crypto perp flow
// over 5min on BTC/ETH typically lands in $1M–$50M territory.
function fmtNotional(v: Num): string {
  if (nullish(v)) return '—'
  const n = v as number
  if (n >= 1_000_000) return `$${(n / 1_000_000).toFixed(2)}M`
  if (n >= 1_000) return `$${(n / 1_000).toFixed(1)}k`
  return `$${n.toFixed(0)}`
}

function fmtImbalance(v: Num): string {
  if (nullish(v)) return '—'
  const n = v as number
  const sign = n >= 0 ? '+' : ''
  return `${sign}${(n * 100).toFixed(1)}%`
}

function imbalanceClass(v: Num): string {
  if (nullish(v)) return 'stat-value--muted'
  const n = v as number
  if (Math.abs(n) < 0.05) return 'stat-value--muted'
  return n > 0 ? 'stat-value--green' : 'stat-value--red'
}

function imbalanceLabel(v: Num): string {
  if (nullish(v)) return ''
  const n = v as number
  if (Math.abs(n) < 0.05) return 'balanced'
  return n > 0 ? 'aggressors lifting offers' : 'aggressors hitting bids'
}

export function FlowPulsePanel({ underlying }: { underlying?: string | null } = {}) {
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
          <span className="panel-title">Flow Pulse</span>
          <span className="panel-badge">5MIN · PERP</span>
        </div>
        <div style={{ padding: '12px 16px', color: 'var(--text-muted)', fontSize: 13 }}>
          Waiting for radar data.
        </div>
      </div>
    )
  }

  const [u, msg] = entry
  const f = msg.flow
  const hasFlow = f && f.tradeCount5m > 0

  return (
    <div className="panel">
      <div className="panel-header">
        <span className="panel-title">Flow Pulse · {u}</span>
        <span className="panel-badge">
          {msg.exchange} · 5MIN · {f?.tradeCount5m ?? 0} trades
        </span>
      </div>
      <div style={{ padding: '12px 16px', display: 'grid', gap: 12 }}>
        {!hasFlow && (
          <div style={{ color: 'var(--text-muted)', fontSize: 12 }}>
            No perp trades in the window yet — waiting for md-gateway MdTrade frames.
          </div>
        )}

        {hasFlow && (
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
                Aggressor imbalance (5m)
              </div>
              <div style={{ display: 'flex', alignItems: 'baseline', gap: 12, marginTop: 4 }}>
                <span style={{ fontSize: 22, fontWeight: 700 }} className={imbalanceClass(f.imbalance5m)}>
                  {fmtImbalance(f.imbalance5m)}
                </span>
                <span style={{ color: 'var(--text-muted)', fontSize: 12 }}>{imbalanceLabel(f.imbalance5m)}</span>
              </div>
            </div>

            <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 12, fontSize: 11 }}>
              <div>
                <div style={{ color: 'var(--text-muted)' }}>Buy notional</div>
                <div className="stat-value stat-value--green" style={{ fontSize: 14, fontWeight: 600 }}>
                  {fmtNotional(f.buyNotional5m)}
                </div>
              </div>
              <div>
                <div style={{ color: 'var(--text-muted)' }}>Sell notional</div>
                <div className="stat-value stat-value--red" style={{ fontSize: 14, fontWeight: 600 }}>
                  {fmtNotional(f.sellNotional5m)}
                </div>
              </div>
            </div>
          </>
        )}
      </div>
    </div>
  )
}
