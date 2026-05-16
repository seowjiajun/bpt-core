// OptionsMakerPanel — strategy-state panel for `kind: "OptionsMaker"`.
//
// Three layers in the panel:
//   (1) Top banner showing the `risk_halted` latch — visible only when set;
//       red when the sanity ceiling has tripped (book_delta blew past the
//       sanity-ceiling multiple of max_hedge_abs_delta).
//   (2) Per-(exchange, underlying) summary table — option-side Greeks, perp
//       hedge position, net book delta, hedge status.
//   (3) Per-underlying active-strikes table — strike / theo / venue mid /
//       dislocation / our bid+ask / position. The "main working view" for
//       an options MM operator.

import type { OptionsMakerStrategyState, OptionsMakerStrike, OptionsMakerUnderlyingState } from '../../types/messages'

const fmt = (v: number, d = 2) => v.toFixed(d)
const fmtSigned = (v: number, d = 4) => (v >= 0 ? '+' : '') + v.toFixed(d)

// Book delta colour: green when neutral, amber when starting to drift,
// red when over the operator's typical hedge threshold (0.05 BTC). The
// threshold here is display-only — the strategy enforces its own gate.
function bookDeltaClass(v: number): string {
  const abs = Math.abs(v)
  if (abs < 0.01) return 'stat-value--green'
  if (abs < 0.05) return 'limit-warn'
  return 'stat-value--red'
}

function UnderlyingRow({ u }: { u: OptionsMakerUnderlyingState }) {
  return (
    <tr>
      <td style={{ fontWeight: 600 }}>{u.underlying}</td>
      <td style={{ color: 'var(--text-muted)' }}>{u.exchange}</td>
      <td className="num">{u.option_count}</td>
      <td className="num">{fmtSigned(u.portfolio_delta)}</td>
      <td className="num">{fmt(u.portfolio_vega, 1)}</td>
      <td className="num">{fmt(u.portfolio_gamma, 4)}</td>
      <td className="num">{fmt(u.portfolio_theta, 2)}</td>
      <td className="num">{fmtSigned(u.perp_position, 6)}</td>
      <td className={`num ${bookDeltaClass(u.book_delta)}`}>{fmtSigned(u.book_delta)}</td>
      <td>
        {u.hedge_in_flight ? (
          <span className="limit-warn">HEDGING</span>
        ) : Math.abs(u.book_delta) < 0.01 ? (
          <span className="stat-value--green">NEUTRAL</span>
        ) : (
          <span className="stat-value--muted">IDLE</span>
        )}
      </td>
    </tr>
  )
}

// Format YYYYMMDD as DDMMM (e.g. 20260518 → 18MAY).
function fmtExpiry(yyyymmdd: number): string {
  const y = Math.floor(yyyymmdd / 10000)
  const m = Math.floor((yyyymmdd % 10000) / 100)
  const d = yyyymmdd % 100
  const months = ['JAN', 'FEB', 'MAR', 'APR', 'MAY', 'JUN', 'JUL', 'AUG', 'SEP', 'OCT', 'NOV', 'DEC']
  return `${d.toString().padStart(2, '0')}${months[m - 1] ?? '???'}${(y % 100).toString().padStart(2, '0')}`
}

// Dislocation = theo - venue_mid, expressed as a multiple of half_spread is
// the meaningful ratio — but we don't have half_spread here, so just show
// the absolute number and let colour signal magnitude relative to venue.
function dislocationClass(diff: number, venueMid: number): string {
  if (venueMid <= 0) return 'stat-value--muted'  // synthetic; no comparison
  const pct = Math.abs(diff) / venueMid
  if (pct < 0.02) return 'stat-value--green'
  if (pct < 0.10) return 'limit-warn'
  return 'stat-value--red'
}

function StrikeRow({ s }: { s: OptionsMakerStrike }) {
  const dislocation = s.theo - s.venue_mid
  const synthetic = s.venue_mid <= 0
  return (
    <tr>
      <td>{fmtExpiry(s.expiry)}</td>
      <td className="num">{fmt(s.strike, 0)}</td>
      <td>{s.is_call ? 'C' : 'P'}</td>
      <td className="num">{fmt(s.theo, 5)}</td>
      <td className="num">{synthetic ? '—' : fmt(s.venue_mid, 5)}</td>
      <td className={`num ${dislocationClass(dislocation, s.venue_mid)}`}>
        {synthetic ? '—' : fmtSigned(dislocation, 5)}
      </td>
      <td className="num">{s.our_bid > 0 ? fmt(s.our_bid, 5) : '—'}</td>
      <td className="num">{s.our_ask > 0 ? fmt(s.our_ask, 5) : '—'}</td>
      <td className={`num ${s.position !== 0 ? 'stat-value--green' : 'stat-value--muted'}`}>
        {s.position !== 0 ? fmtSigned(s.position, 4) : '—'}
      </td>
      <td className="num">{fmtSigned(s.delta, 3)}</td>
    </tr>
  )
}

function StrikesTable({ u }: { u: OptionsMakerUnderlyingState }) {
  const strikes = u.active_strikes ?? []
  if (strikes.length === 0) return null
  // Sort by expiry then strike for readability — operators read top-to-bottom.
  const sorted = [...strikes].sort((a, b) => a.expiry - b.expiry || a.strike - b.strike)
  return (
    <div style={{ marginTop: 8 }}>
      <div style={{ padding: '4px 16px', fontSize: 11, color: 'var(--text-muted)' }}>
        {u.underlying} · {strikes.length} active strikes
      </div>
      <table className="blotter-table" style={{ fontSize: 11 }}>
        <thead>
          <tr>
            <th>Expiry</th>
            <th className="num">K</th>
            <th>C/P</th>
            <th className="num">Theo</th>
            <th className="num">Venue mid</th>
            <th className="num">Δ vs mid</th>
            <th className="num">Our bid</th>
            <th className="num">Our ask</th>
            <th className="num">Pos</th>
            <th className="num">Δ</th>
          </tr>
        </thead>
        <tbody>
          {sorted.map((s) => (
            <StrikeRow key={`${s.expiry}-${s.strike}-${s.is_call ? 'C' : 'P'}`} s={s} />
          ))}
        </tbody>
      </table>
    </div>
  )
}

export function OptionsMakerPanel({ state: ss }: { state: OptionsMakerStrategyState }) {
  const rows = ss.underlyings ?? []
  const anyHedging = rows.some((u) => u.hedge_in_flight)
  const totalOptions = rows.reduce((s, u) => s + u.option_count, 0)

  return (
    <div className="panel" style={{ gridArea: 'stratstate' }}>
      <div className="panel-header">
        <span className="panel-title">Strategy State</span>
        <span className="panel-badge">
          OptionsMaker · {rows.length} ul · {totalOptions} strikes
          {anyHedging && <span className="limit-warn"> · HEDGING</span>}
          {ss.risk_halted && <span className="stat-value--red"> · HALTED</span>}
        </span>
      </div>

      {ss.risk_halted && (
        <div
          style={{
            padding: '8px 16px',
            background: 'var(--color-red-bg, #4a1010)',
            color: 'var(--color-red-fg, #ff6666)',
            fontSize: 12,
            fontWeight: 600,
            borderTop: '1px solid var(--color-red-fg, #ff6666)',
            borderBottom: '1px solid var(--color-red-fg, #ff6666)',
          }}
        >
          RISK HALTED — book-delta sanity ceiling tripped. Strategy is not quoting or hedging.
          Restart process after reviewing positions.
        </div>
      )}

      {rows.length === 0 ? (
        <div style={{ padding: '12px 16px', color: 'var(--text-muted)', fontSize: 13 }}>
          Strategy started — waiting for first VolSurface frame.
        </div>
      ) : (
        <>
          <table className="blotter-table" style={{ fontSize: 11 }}>
            <thead>
              <tr>
                <th>UL</th>
                <th>Venue</th>
                <th className="num">Strikes</th>
                <th className="num">Δ-opts</th>
                <th className="num">Vega</th>
                <th className="num">Γ</th>
                <th className="num">Θ</th>
                <th className="num">Perp pos</th>
                <th className="num">Book Δ</th>
                <th>Hedge</th>
              </tr>
            </thead>
            <tbody>
              {rows.map((u) => (
                <UnderlyingRow key={`${u.exchange}:${u.underlying}`} u={u} />
              ))}
            </tbody>
          </table>
          {rows.map((u) => (
            <StrikesTable key={`strikes:${u.exchange}:${u.underlying}`} u={u} />
          ))}
        </>
      )}
    </div>
  )
}
