// Vol Regime — renders the realized-vol portion of bpt-radar's MarketColor
// snapshot and joins it with the surface's front ATM IV to compute the
// variance risk premium (RV − IV).
//
// RV is the 1h annualised realised vol of the joined perp's mid. IV is the
// front-expiry ATM implied vol already published in the options section.
// VRP positive = realised exceeded implied → vol-buyers got paid in the
// last hour; VRP negative = implied exceeded realised → vol-sellers got paid.
//
// Classifier lives in this file (not radar) so the labels / cutoffs can
// evolve without churning the C++ wire schema.

import { useStore } from '../store'
import type { MarketColorMsg } from '../types/messages'

type Num = number | null | undefined
const nullish = (v: Num): v is null | undefined => v == null || isNaN(v as number)

const fmtVolPct = (v: Num) => (nullish(v) ? '—' : `${((v as number) * 100).toFixed(1)}%`)

// Classify the RV/IV gap. The thresholds are crypto-perp shaped — RV/IV ratios
// outside [0.7, 1.3] are operator-actionable; inside is normal-state noise.
type Regime = { label: string; cls: string; hint: string }
function classifyRegime(rv: Num, iv: Num): Regime {
  if (nullish(rv)) return { label: 'warming up', cls: 'stat-value--muted', hint: 'collecting mid samples' }
  if (nullish(iv)) return { label: 'no IV reference', cls: 'stat-value--muted', hint: 'need front ATM IV from surface' }
  const ratio = (rv as number) / (iv as number)
  if (ratio < 0.7) return { label: 'IV crushed', cls: 'stat-value--green', hint: 'options expensive vs realised — vol seller setup' }
  if (ratio < 0.9) return { label: 'IV premium', cls: 'stat-value--green', hint: 'normal — IV above RV' }
  if (ratio <= 1.1) return { label: 'balanced', cls: 'stat-value--muted', hint: 'RV ≈ IV — neither side getting paid' }
  if (ratio <= 1.5) return { label: 'RV premium', cls: 'stat-value--red', hint: 'realised above IV — vol buyer setup' }
  return { label: 'stressed', cls: 'stat-value--red', hint: 'RV >> IV — regime break, size down quotes' }
}

function fmtVrp(v: Num): string {
  if (nullish(v)) return '—'
  const n = v as number
  const sign = n >= 0 ? '+' : ''
  return `${sign}${(n * 100).toFixed(2)} vol pts`
}

function vrpClass(v: Num): string {
  if (nullish(v)) return 'stat-value--muted'
  const n = v as number
  if (Math.abs(n) < 0.02) return 'stat-value--muted'
  return n > 0 ? 'stat-value--red' : 'stat-value--green'
}

export function VolRegimePanel({ underlying }: { underlying?: string | null } = {}) {
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
          <span className="panel-title">Vol Regime</span>
          <span className="panel-badge">RV · 1H</span>
        </div>
        <div style={{ padding: '12px 16px', color: 'var(--text-muted)', fontSize: 13 }}>
          Waiting for radar data.
        </div>
      </div>
    )
  }

  const [u, msg] = entry
  const rv = msg.regime?.realizedVol1h
  const iv = msg.options?.frontAtmIv
  const vrp = !nullish(rv) && !nullish(iv) ? (rv as number) - (iv as number) : null
  const regime = classifyRegime(rv, iv)

  return (
    <div className="panel">
      <div className="panel-header">
        <span className="panel-title">Vol Regime · {u}</span>
        <span className="panel-badge">
          {msg.exchange} · 1H · {msg.regime?.sampleCount ?? 0} samples
        </span>
      </div>
      <div style={{ padding: '12px 16px', display: 'grid', gap: 12 }}>
        <div>
          <div
            style={{
              color: 'var(--text-muted)',
              fontSize: 11,
              textTransform: 'uppercase',
              letterSpacing: 1,
            }}
          >
            Regime
          </div>
          <div style={{ display: 'flex', alignItems: 'baseline', gap: 12, marginTop: 4 }}>
            <span style={{ fontSize: 22, fontWeight: 700 }} className={regime.cls}>
              {regime.label}
            </span>
            <span style={{ color: 'var(--text-muted)', fontSize: 12 }}>{regime.hint}</span>
          </div>
        </div>

        <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr 1fr', gap: 12, fontSize: 11 }}>
          <div>
            <div style={{ color: 'var(--text-muted)' }}>Realized vol (1h)</div>
            <div className="stat-value" style={{ fontSize: 14, fontWeight: 600 }}>
              {fmtVolPct(rv)}
            </div>
          </div>
          <div>
            <div style={{ color: 'var(--text-muted)' }}>ATM IV (front)</div>
            <div className="stat-value" style={{ fontSize: 14, fontWeight: 600 }}>
              {fmtVolPct(iv)}
            </div>
          </div>
          <div>
            <div style={{ color: 'var(--text-muted)' }}>VRP (RV − IV)</div>
            <div className={`stat-value ${vrpClass(vrp)}`} style={{ fontSize: 14, fontWeight: 600 }}>
              {fmtVrp(vrp)}
            </div>
          </div>
        </div>
      </div>
    </div>
  )
}
