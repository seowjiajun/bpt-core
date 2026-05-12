import { useStore } from '../store'

type Num = number | null | undefined
const nullish = (v: Num): v is null | undefined => v == null || isNaN(v as number)

const fmt1 = (v: Num) => (nullish(v) ? '—' : (v as number).toFixed(1))
const fmtPct = (v: Num) => (nullish(v) ? '—' : `${((v as number) * 100).toFixed(0)}%`)
const fmtScore = (v: Num) => (nullish(v) ? '—' : (v as number).toFixed(2))
const fmtMs = (v: Num) => (nullish(v) ? '—' : `${(v as number).toFixed(0)}ms`)

function scoreClass(v: Num): string {
  if (nullish(v)) return 'stat-value--muted'
  if ((v as number) > 0) return 'stat-value--green'
  if ((v as number) > -2) return 'stat-value--muted'
  return 'stat-value--red'
}

function rateClass(v: Num): string {
  if (nullish(v)) return 'stat-value--muted'
  if ((v as number) < 0.4) return 'stat-value--green'
  if ((v as number) < 0.6) return 'stat-value--muted'
  return 'stat-value--red'
}

function fillRateClass(v: Num): string {
  if (nullish(v)) return 'stat-value--muted'
  if ((v as number) > 0.8) return 'stat-value--red'
  if ((v as number) > 0.5) return 'stat-value--muted'
  return 'stat-value--green'
}

function ttfClass(v: Num): string {
  if (nullish(v)) return 'stat-value--muted'
  if ((v as number) < 500) return 'stat-value--red'
  if ((v as number) < 2000) return 'stat-value--muted'
  return 'stat-value--green'
}

// Sample threshold below which markout / adverse / score are statistically
// meaningless — they'd render as 0.0 which is indistinguishable from "flat
// market" but actually means "we only have N<5 fills, trust nothing." The
// number matches scorer_min_samples = 5 in bpt-analytics/config/tyr.*.toml.
// Plumbing the exact threshold through the message would be more correct
// but the config rarely changes; hardcoding keeps the panel self-contained.
const MIN_SAMPLES = 5

export function ToxicityPanel() {
  const tox = useStore((s) => s.toxicity)

  if (!tox) {
    return (
      <div className="panel" style={{ gridArea: 'toxicity' }}>
        <div className="panel-header">
          <span className="panel-title">Flow Toxicity</span>
          <span className="panel-badge">TOX</span>
        </div>
        <div style={{ padding: '12px 16px', color: 'var(--text-muted)', fontSize: 13 }}>
          Waiting for analytics data.
        </div>
      </div>
    )
  }

  // Per-side warmup gate: when samples are sparse, mask the markout /
  // adverse / score cells with '—' rather than letting a single-sample
  // zero read as "flat market." Fill rate + TTF + sample count stay
  // visible because they converge meaningfully even at n=1.
  const bidWarmup = tox.bidSamples < MIN_SAMPLES
  const askWarmup = tox.askSamples < MIN_SAMPLES
  const mask = (v: Num, warmup: boolean): Num => (warmup ? null : v)

  const bothWarm = !bidWarmup && !askWarmup
  const badgeText = bothWarm
    ? `TOX · ${tox.bidSamples + tox.askSamples} fills`
    : `WARMUP · bid ${tox.bidSamples}/${MIN_SAMPLES} ask ${tox.askSamples}/${MIN_SAMPLES}`
  const badgeClass = bothWarm ? 'panel-badge' : 'panel-badge panel-badge--warn'

  return (
    <div className="panel" style={{ gridArea: 'toxicity' }}>
      <div className="panel-header">
        <span className="panel-title">Flow Toxicity</span>
        <span className={badgeClass}>{badgeText}</span>
      </div>
      <table className="blotter-table" style={{ fontSize: 11 }}>
        <thead>
          <tr>
            <th></th>
            <th className="num">Markout 5s</th>
            <th className="num">Adverse %</th>
            <th className="num">Score</th>
            <th className="num">Fill Rate</th>
            <th className="num">TTF</th>
          </tr>
        </thead>
        <tbody>
          <tr>
            <td style={{ fontWeight: 600 }}>BID</td>
            <td className={`num ${scoreClass(mask(tox.bidMarkout5s, bidWarmup))}`}>
              {fmt1(mask(tox.bidMarkout5s, bidWarmup))} bps
            </td>
            <td className={`num ${rateClass(mask(tox.bidAdverseRate, bidWarmup))}`}>
              {fmtPct(mask(tox.bidAdverseRate, bidWarmup))}
            </td>
            <td className={`num ${scoreClass(mask(tox.bidToxScore, bidWarmup))}`}>
              {fmtScore(mask(tox.bidToxScore, bidWarmup))}
            </td>
            <td className={`num ${fillRateClass(tox.bidFillRate)}`}>{fmtPct(tox.bidFillRate)}</td>
            <td className={`num ${ttfClass(tox.bidTtfMs)}`}>{fmtMs(tox.bidTtfMs)}</td>
          </tr>
          <tr>
            <td style={{ fontWeight: 600 }}>ASK</td>
            <td className={`num ${scoreClass(mask(tox.askMarkout5s, askWarmup))}`}>
              {fmt1(mask(tox.askMarkout5s, askWarmup))} bps
            </td>
            <td className={`num ${rateClass(mask(tox.askAdverseRate, askWarmup))}`}>
              {fmtPct(mask(tox.askAdverseRate, askWarmup))}
            </td>
            <td className={`num ${scoreClass(mask(tox.askToxScore, askWarmup))}`}>
              {fmtScore(mask(tox.askToxScore, askWarmup))}
            </td>
            <td className={`num ${fillRateClass(tox.askFillRate)}`}>{fmtPct(tox.askFillRate)}</td>
            <td className={`num ${ttfClass(tox.askTtfMs)}`}>{fmtMs(tox.askTtfMs)}</td>
          </tr>
        </tbody>
      </table>
    </div>
  )
}
