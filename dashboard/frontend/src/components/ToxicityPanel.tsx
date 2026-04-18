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

export function ToxicityPanel() {
  const tox = useStore((s) => s.toxicity)

  if (!tox) {
    return (
      <div className="panel" style={{ gridArea: 'toxicity' }}>
        <div className="panel-header">
          <span className="panel-title">Flow Toxicity</span>
          <span className="panel-badge">TYR</span>
        </div>
        <div style={{ padding: '12px 16px', color: 'var(--text-muted)', fontSize: 13 }}>
          Waiting for Tyr data.
        </div>
      </div>
    )
  }

  return (
    <div className="panel" style={{ gridArea: 'toxicity' }}>
      <div className="panel-header">
        <span className="panel-title">Flow Toxicity</span>
        <span className="panel-badge">TYR · {tox.bidSamples + tox.askSamples} fills</span>
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
            <td className={`num ${scoreClass(tox.bidMarkout5s)}`}>
              {fmt1(tox.bidMarkout5s)} bps
            </td>
            <td className={`num ${rateClass(tox.bidAdverseRate)}`}>
              {fmtPct(tox.bidAdverseRate)}
            </td>
            <td className={`num ${scoreClass(tox.bidToxScore)}`}>
              {fmtScore(tox.bidToxScore)}
            </td>
            <td className={`num ${fillRateClass(tox.bidFillRate)}`}>
              {fmtPct(tox.bidFillRate)}
            </td>
            <td className={`num ${ttfClass(tox.bidTtfMs)}`}>
              {fmtMs(tox.bidTtfMs)}
            </td>
          </tr>
          <tr>
            <td style={{ fontWeight: 600 }}>ASK</td>
            <td className={`num ${scoreClass(tox.askMarkout5s)}`}>
              {fmt1(tox.askMarkout5s)} bps
            </td>
            <td className={`num ${rateClass(tox.askAdverseRate)}`}>
              {fmtPct(tox.askAdverseRate)}
            </td>
            <td className={`num ${scoreClass(tox.askToxScore)}`}>
              {fmtScore(tox.askToxScore)}
            </td>
            <td className={`num ${fillRateClass(tox.askFillRate)}`}>
              {fmtPct(tox.askFillRate)}
            </td>
            <td className={`num ${ttfClass(tox.askTtfMs)}`}>
              {fmtMs(tox.askTtfMs)}
            </td>
          </tr>
        </tbody>
      </table>
    </div>
  )
}
