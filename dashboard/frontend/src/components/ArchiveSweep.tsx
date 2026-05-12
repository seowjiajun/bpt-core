import { useEffect, useMemo, useState } from 'react'

// Sweep view: pick 3+ runs, the dashboard introspects each run's
// params.toml to find which leaf params differ across the selection,
// then renders the chosen metric across that varied dimension.
//
// 1 dimension (e.g. only `gamma` differs) → bar/line chart
// 2 dimensions (e.g. `gamma` × `kappa`)   → heatmap grid
// 3+ dimensions or unparseable params     → fall back to a flat table
//
// Reached via location.hash:
//   /archive#sweep:<runA>:<runB>:<runC>:...
// Run names are URL-encoded.

interface MarkoutHorizon {
  resolved_fills: number
  avg_bps: number
}

interface Summary {
  starting_capital: number
  final_equity: number
  total_pnl: number
  return_pct: number
  max_drawdown_pct: number
  sharpe_per_fill: number
  total_fills: number
  win_rate_pct: number
  buy_count?: number
  sell_count?: number
  markouts?: {
    '50ms'?: MarkoutHorizon
    '1s'?: MarkoutHorizon
    '5s'?: MarkoutHorizon
    '30s'?: MarkoutHorizon
  }
  round_trips?: { round_trip_win_rate_pct?: number; avg_holding_ms?: number }
  strategy_name?: string
  params_hash?: string
  git_sha?: string
}

interface RunDetail {
  name: string
  summary: Summary | null
  params: Record<string, unknown> | null
}

interface Props {
  runs: string[]
}

// Available metrics to colour the heatmap by. Keep the list small so
// the dropdown is glanceable; markouts and round-trip win rate cover
// the AS-relevant questions without overwhelming.
type MetricKey =
  | 'total_pnl'
  | 'sharpe_per_fill'
  | 'win_rate_pct'
  | 'total_fills'
  | 'markout_30s_bps'
  | 'rt_win_rate_pct'
  | 'avg_holding_ms'
const metricLabels: Record<MetricKey, string> = {
  total_pnl: 'Total PnL ($)',
  sharpe_per_fill: 'Sharpe / fill',
  win_rate_pct: 'Win rate (%)',
  total_fills: 'Fill count',
  markout_30s_bps: 'Markout 30s (bps)',
  rt_win_rate_pct: 'RT win rate (%)',
  avg_holding_ms: 'Avg holding (ms)',
}
// "higher is better" governs how the colour gradient maps. False ⇒
// invert (so red is high, green is low).
const metricHigherBetter: Record<MetricKey, boolean> = {
  total_pnl: true,
  sharpe_per_fill: true,
  win_rate_pct: true,
  total_fills: true,
  markout_30s_bps: true,
  rt_win_rate_pct: true,
  avg_holding_ms: false,
}

function metricValue(s: Summary | null, k: MetricKey): number | undefined {
  if (!s) return undefined
  switch (k) {
    case 'total_pnl':
      return s.total_pnl
    case 'sharpe_per_fill':
      return s.sharpe_per_fill
    case 'win_rate_pct':
      return s.win_rate_pct
    case 'total_fills':
      return s.total_fills
    case 'markout_30s_bps':
      return s.markouts?.['30s']?.avg_bps
    case 'rt_win_rate_pct':
      return s.round_trips?.round_trip_win_rate_pct
    case 'avg_holding_ms':
      return s.round_trips?.avg_holding_ms
  }
}

// Walk a parsed-TOML object and yield `("a.b.c", value)` for every
// scalar leaf. Nested tables expand dot-joined; arrays come through
// stringified so the diff treats `[1,2]` vs `[1,3]` as different.
function* leafIter(obj: unknown, prefix: string[] = []): Generator<[string, string]> {
  if (obj === null || typeof obj !== 'object' || Array.isArray(obj)) {
    yield [prefix.join('.'), JSON.stringify(obj)]
    return
  }
  for (const [k, v] of Object.entries(obj as Record<string, unknown>)) {
    yield* leafIter(v, [...prefix, k])
  }
}

function paramsToFlatMap(p: Record<string, unknown> | null): Map<string, string> {
  const m = new Map<string, string>()
  if (!p) return m
  for (const [k, v] of leafIter(p)) m.set(k, v)
  return m
}

// Heatmap colouring — interpolate red→amber→green across the value
// range, inverting if higherIsBetter is false.
function colour(v: number | undefined, lo: number, hi: number, higherBetter: boolean): string {
  if (v === undefined) return 'transparent'
  if (hi === lo) return 'rgba(56,139,253,0.18)'
  let t = (v - lo) / (hi - lo)
  if (!higherBetter) t = 1 - t
  // 0 = red, 0.5 = amber, 1 = green
  const r = t < 0.5 ? 248 : Math.round(248 - (248 - 63) * (t - 0.5) * 2)
  const g =
    t < 0.5 ? Math.round(63 + (210 - 63) * t * 2) : Math.round(210 - (210 - 185) * (t - 0.5) * 2)
  const b =
    t < 0.5 ? Math.round(73 + (110 - 73) * t * 2) : Math.round(110 - (110 - 80) * (t - 0.5) * 2)
  return `rgba(${r},${g},${b},0.32)`
}

export function ArchiveSweep({ runs }: Props) {
  const [details, setDetails] = useState<RunDetail[] | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [metric, setMetric] = useState<MetricKey>('total_pnl')

  useEffect(() => {
    setDetails(null)
    setError(null)
    Promise.all(
      runs.map((r) =>
        fetch(`/api/backtest-runs/${encodeURIComponent(r)}`).then((res) => res.json())
      )
    )
      .then((all: RunDetail[]) => setDetails(all))
      .catch((e) => setError(String(e)))
  }, [runs])

  const analysis = useMemo(() => {
    if (!details) return null
    // Find leaf params that vary across the selection. Anything constant
    // is uninteresting and would just clutter the heatmap axes.
    const flatPerRun = details.map((d) => paramsToFlatMap(d.params))
    const allKeys = new Set<string>()
    for (const m of flatPerRun) for (const k of m.keys()) allKeys.add(k)
    const varying: string[] = []
    for (const k of allKeys) {
      const seen = new Set<string>()
      for (const m of flatPerRun) seen.add(m.get(k) ?? '<missing>')
      if (seen.size > 1) varying.push(k)
    }
    return { flatPerRun, varying }
  }, [details])

  if (error)
    return (
      <div className="archive-body" style={{ padding: 16, color: 'var(--red)' }}>
        Error: {error}
      </div>
    )
  if (!details || !analysis)
    return (
      <div className="archive-body" style={{ padding: 16, color: 'var(--text-muted)' }}>
        Loading {runs.length} runs…
      </div>
    )

  const { flatPerRun, varying } = analysis

  // Render decision: 1 dim → bar chart-style table, 2 dims → heatmap,
  // else flat table with all varying columns.
  const dim = varying.length

  const values = details.map((d) => metricValue(d.summary, metric))
  const finite = values.filter((v): v is number => v !== undefined && Number.isFinite(v))
  const lo = finite.length ? Math.min(...finite) : 0
  const hi = finite.length ? Math.max(...finite) : 0
  const fmt = (v: number | undefined) =>
    v === undefined
      ? '—'
      : metric === 'total_fills'
        ? v.toFixed(0)
        : Math.abs(v) >= 100
          ? v.toFixed(0)
          : v.toFixed(3)

  return (
    <div className="archive-body" style={{ display: 'grid', gridTemplateRows: '1fr' }}>
      <div className="panel" style={{ overflow: 'auto' }}>
        <div className="panel-header">
          <span className="panel-title">Sweep ({details.length} runs)</span>
          <span
            className="panel-badge"
            style={{ flex: 1, textAlign: 'left', marginLeft: 16, color: 'var(--text-muted)' }}
          >
            {dim === 0 && 'no varying params — selected runs share identical params'}
            {dim === 1 && `1-D sweep over ${varying[0]}`}
            {dim === 2 && `2-D sweep over ${varying[0]} × ${varying[1]}`}
            {dim >= 3 && `${dim} dimensions vary — narrow the selection or accept the flat table`}
          </span>
          <select
            value={metric}
            onChange={(e) => setMetric(e.target.value as MetricKey)}
            style={{
              fontFamily: 'var(--font-mono)',
              fontSize: 11,
              background: 'var(--bg-panel)',
              color: 'var(--text-primary)',
              border: '1px solid var(--border)',
              borderRadius: 3,
              padding: '4px 8px',
            }}
          >
            {Object.entries(metricLabels).map(([k, label]) => (
              <option key={k} value={k}>
                {label}
              </option>
            ))}
          </select>
        </div>
        <div style={{ padding: 12, overflow: 'auto' }}>
          {dim === 0 && (
            <p style={{ color: 'var(--text-muted)' }}>
              All {details.length} runs use identical params — a sweep needs at least one varied
              dimension. Try selecting runs from different sweep iterations.
            </p>
          )}

          {dim === 1 && (
            <OneDimSweep
              runs={details}
              flat={flatPerRun}
              varied={varying[0]}
              metric={metric}
              metricLabel={metricLabels[metric]}
              higherBetter={metricHigherBetter[metric]}
              lo={lo}
              hi={hi}
              fmt={fmt}
            />
          )}

          {dim === 2 && (
            <TwoDimSweep
              runs={details}
              flat={flatPerRun}
              varied={[varying[0], varying[1]]}
              metric={metric}
              higherBetter={metricHigherBetter[metric]}
              lo={lo}
              hi={hi}
              fmt={fmt}
            />
          )}

          {dim >= 3 && (
            <FlatTable
              runs={details}
              flat={flatPerRun}
              varied={varying}
              metric={metric}
              higherBetter={metricHigherBetter[metric]}
              lo={lo}
              hi={hi}
              fmt={fmt}
            />
          )}
        </div>
      </div>
    </div>
  )
}

// ── 1D sweep ────────────────────────────────────────────────────────────────
function OneDimSweep(props: {
  runs: RunDetail[]
  flat: Map<string, string>[]
  varied: string
  metric: MetricKey
  metricLabel: string
  higherBetter: boolean
  lo: number
  hi: number
  fmt: (v: number | undefined) => string
}) {
  const { runs, flat, varied, metric, metricLabel, higherBetter, lo, hi, fmt } = props
  const rows = runs.map((r, i) => ({
    run: r,
    paramVal: flat[i].get(varied) ?? '—',
    val: metricValue(r.summary, metric),
  }))
  // Try to sort numerically by param value; fall back to string sort.
  rows.sort((a, b) => {
    const an = Number(a.paramVal)
    const bn = Number(b.paramVal)
    if (Number.isFinite(an) && Number.isFinite(bn)) return an - bn
    return a.paramVal.localeCompare(b.paramVal)
  })
  return (
    <table className="blotter-table" style={{ width: '100%' }}>
      <thead>
        <tr>
          <th>{varied}</th>
          <th className="num">{metricLabel}</th>
          <th>Run</th>
        </tr>
      </thead>
      <tbody>
        {rows.map((r) => (
          <tr key={r.run.name} style={{ background: colour(r.val, lo, hi, higherBetter) }}>
            <td className="mono-7" style={{ fontWeight: 600 }}>
              {r.paramVal}
            </td>
            <td className="num">{fmt(r.val)}</td>
            <td className="mono-7" style={{ color: 'var(--text-muted)' }}>
              {r.run.name}
            </td>
          </tr>
        ))}
      </tbody>
    </table>
  )
}

// ── 2D heatmap ──────────────────────────────────────────────────────────────
function TwoDimSweep(props: {
  runs: RunDetail[]
  flat: Map<string, string>[]
  varied: [string, string]
  metric: MetricKey
  higherBetter: boolean
  lo: number
  hi: number
  fmt: (v: number | undefined) => string
}) {
  const { runs, flat, varied, metric, higherBetter, lo, hi, fmt } = props
  const [xKey, yKey] = varied

  const xs = Array.from(new Set(flat.map((m) => m.get(xKey) ?? '—'))).sort(numAwareCmp)
  const ys = Array.from(new Set(flat.map((m) => m.get(yKey) ?? '—'))).sort(numAwareCmp)

  // Lookup: (x,y) → run summary metric. If the user picked a non-grid
  // selection, some cells stay empty.
  const cell = new Map<string, { val: number | undefined; runName: string }>()
  for (let i = 0; i < runs.length; ++i) {
    const x = flat[i].get(xKey) ?? '—'
    const y = flat[i].get(yKey) ?? '—'
    cell.set(`${x}|${y}`, { val: metricValue(runs[i].summary, metric), runName: runs[i].name })
  }

  return (
    <table className="blotter-table" style={{ width: 'auto' }}>
      <thead>
        <tr>
          <th>
            {yKey} \ {xKey}
          </th>
          {xs.map((x) => (
            <th key={x} className="num mono-7">
              {x}
            </th>
          ))}
        </tr>
      </thead>
      <tbody>
        {ys.map((y) => (
          <tr key={y}>
            <td className="mono-7" style={{ fontWeight: 600 }}>
              {y}
            </td>
            {xs.map((x) => {
              const c = cell.get(`${x}|${y}`)
              return (
                <td
                  key={x}
                  className="num"
                  title={c?.runName ?? 'no run for this cell'}
                  style={{
                    background: colour(c?.val, lo, hi, higherBetter),
                    padding: '8px 12px',
                    minWidth: 80,
                  }}
                >
                  {fmt(c?.val)}
                </td>
              )
            })}
          </tr>
        ))}
      </tbody>
    </table>
  )
}

function numAwareCmp(a: string, b: string): number {
  const an = Number(a)
  const bn = Number(b)
  if (Number.isFinite(an) && Number.isFinite(bn)) return an - bn
  return a.localeCompare(b)
}

// ── 3+ dims: just a flat table with every varied column shown ────────────────
function FlatTable(props: {
  runs: RunDetail[]
  flat: Map<string, string>[]
  varied: string[]
  metric: MetricKey
  higherBetter: boolean
  lo: number
  hi: number
  fmt: (v: number | undefined) => string
}) {
  const { runs, flat, varied, metric, higherBetter, lo, hi, fmt } = props
  return (
    <table className="blotter-table" style={{ width: '100%' }}>
      <thead>
        <tr>
          {varied.map((k) => (
            <th key={k}>{k}</th>
          ))}
          <th className="num">metric</th>
          <th>Run</th>
        </tr>
      </thead>
      <tbody>
        {runs.map((r, i) => {
          const v = metricValue(r.summary, metric)
          return (
            <tr key={r.name} style={{ background: colour(v, lo, hi, higherBetter) }}>
              {varied.map((k) => (
                <td key={k} className="mono-7">
                  {flat[i].get(k) ?? '—'}
                </td>
              ))}
              <td className="num">{fmt(v)}</td>
              <td className="mono-7" style={{ color: 'var(--text-muted)' }}>
                {r.name}
              </td>
            </tr>
          )
        })}
      </tbody>
    </table>
  )
}
