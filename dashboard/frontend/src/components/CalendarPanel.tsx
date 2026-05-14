// Calendar — event-aware ops panel. Today renders only the options expiry
// countdown (cheap: all data already in MarketColor). Funding countdown +
// macro events plug in here as the radar service grows.
//
// Why this matters: pinning effect is real in the last 24-48h of quarterly
// expiries (spot gravitates toward max pain). Operators need to know how
// close they are without doing the YYYYMMDD math in their head every glance.

import { useEffect, useState } from 'react'
import { useStore } from '../store'
import type { MarketColorMsg } from '../types/messages'

// Re-render the countdown every second so the displayed "23h 14m" stays fresh
// without waiting for the next 2s radar publish.
function useNowMs() {
  const [now, setNow] = useState(Date.now())
  useEffect(() => {
    const id = setInterval(() => setNow(Date.now()), 1000)
    return () => clearInterval(id)
  }, [])
  return now
}

// YYYYMMDD → epoch ms at 08:00 UTC. Deribit's quarterly + monthly options
// all settle at 08:00 UTC on the expiry date; daily/weekly options on
// Deribit also settle at 08:00 UTC. Hardcoded venue convention; revisit
// if we add venues with different settlement times.
function expiryToMs(yyyymmdd: number): number {
  if (!yyyymmdd) return 0
  const y = Math.floor(yyyymmdd / 10000)
  const m = Math.floor((yyyymmdd % 10000) / 100) - 1
  const d = yyyymmdd % 100
  return Date.UTC(y, m, d, 8, 0, 0)
}

function formatCountdown(deltaMs: number): { text: string; cls: string } {
  if (deltaMs <= 0) return { text: 'expired', cls: 'stat-value--muted' }
  const s = Math.floor(deltaMs / 1000)
  const days = Math.floor(s / 86400)
  const hours = Math.floor((s % 86400) / 3600)
  const mins = Math.floor((s % 3600) / 60)
  const secs = s % 60

  // Color escalates as expiry approaches — last 48h is the pinning zone where
  // the operator should bias quotes / size toward max pain.
  let cls = 'stat-value--muted'
  if (deltaMs < 48 * 3600 * 1000) cls = 'stat-value--red'
  else if (deltaMs < 7 * 86400 * 1000) cls = 'stat-value--green'

  if (days > 0) return { text: `${days}d ${hours}h ${mins}m`, cls }
  if (hours > 0) return { text: `${hours}h ${mins}m ${secs}s`, cls }
  return { text: `${mins}m ${secs}s`, cls }
}

function fmtPx(v: number | null | undefined): string {
  if (v == null || !Number.isFinite(v)) return '—'
  return v.toLocaleString(undefined, { maximumFractionDigits: 2 })
}

export function CalendarPanel({ underlying }: { underlying?: string | null } = {}) {
  const marketColor = useStore((s) => s.marketColor)
  const now = useNowMs()

  const entry: [string, MarketColorMsg] | undefined = marketColor
    ? underlying && marketColor[underlying]
      ? [underlying, marketColor[underlying]]
      : (Object.entries(marketColor).sort(([a], [b]) => a.localeCompare(b))[0] as [string, MarketColorMsg] | undefined)
    : undefined

  if (!entry) {
    return (
      <div className="panel">
        <div className="panel-header">
          <span className="panel-title">Calendar</span>
          <span className="panel-badge">EVENTS</span>
        </div>
        <div style={{ padding: '12px 16px', color: 'var(--text-muted)', fontSize: 13 }}>
          Waiting for radar data.
        </div>
      </div>
    )
  }

  const [u, msg] = entry
  const o = msg.options

  const expiryMs = expiryToMs(o.frontExpiry)
  const expiryDelta = expiryMs - now
  const expiryFmt = formatCountdown(expiryDelta)

  // YYYYMMDD → human ISO date for the title
  const isoExpiry =
    o.frontExpiry > 0
      ? `${Math.floor(o.frontExpiry / 10000)}-${String(Math.floor((o.frontExpiry % 10000) / 100)).padStart(2, '0')}-${String(o.frontExpiry % 100).padStart(2, '0')}`
      : '—'

  const inPinningZone = expiryDelta > 0 && expiryDelta < 48 * 3600 * 1000

  return (
    <div className="panel">
      <div className="panel-header">
        <span className="panel-title">Calendar · {u}</span>
        <span className="panel-badge">EVENTS</span>
      </div>
      <div style={{ padding: '12px 16px', display: 'grid', gap: 12 }}>
        {/* Front options expiry — the load-bearing one */}
        <div>
          <div style={{ color: 'var(--text-muted)', fontSize: 11, textTransform: 'uppercase', letterSpacing: 1 }}>
            Next options expiry
          </div>
          <div style={{ display: 'flex', alignItems: 'baseline', gap: 12, marginTop: 4 }}>
            <span style={{ fontSize: 22, fontWeight: 700 }} className={expiryFmt.cls}>
              {expiryFmt.text}
            </span>
            <span style={{ color: 'var(--text-muted)', fontSize: 13 }}>
              {isoExpiry} 08:00 UTC · max pain {fmtPx(o.maxPainStrike)}
            </span>
          </div>
          {inPinningZone && (
            <div
              style={{
                marginTop: 6,
                padding: '6px 10px',
                background: 'rgba(239, 68, 68, 0.12)',
                color: 'var(--text)',
                fontSize: 12,
                borderRadius: 4,
                border: '1px solid rgba(239, 68, 68, 0.4)',
              }}
            >
              <strong>Pinning zone</strong> — spot tends to drift toward max pain in the last 48h. Bias quote
              size / direction accordingly.
            </div>
          )}
        </div>

        {/* Funding countdown — placeholder. Lights up when radar publishes
            perp_funding_rate_8h and perp_next_funding_ts (Phase B/C of the
            calendar roadmap). */}
        <div>
          <div style={{ color: 'var(--text-muted)', fontSize: 11, textTransform: 'uppercase', letterSpacing: 1 }}>
            Next funding tick
          </div>
          <div style={{ display: 'flex', alignItems: 'baseline', gap: 12, marginTop: 4 }}>
            <span style={{ fontSize: 22, fontWeight: 700, color: 'var(--text-muted)' }}>—</span>
            <span style={{ color: 'var(--text-muted)', fontSize: 12 }}>
              (waiting for radar to plumb funding rate from md-gateway)
            </span>
          </div>
        </div>

        {/* Macro events placeholder — separate scope (ICS feed integration). */}
        <div>
          <div style={{ color: 'var(--text-muted)', fontSize: 11, textTransform: 'uppercase', letterSpacing: 1 }}>
            Macro events
          </div>
          <div style={{ color: 'var(--text-muted)', fontSize: 12, marginTop: 4 }}>
            FOMC / CPI / NFP — not wired yet (external calendar feed required).
          </div>
        </div>
      </div>
    </div>
  )
}
