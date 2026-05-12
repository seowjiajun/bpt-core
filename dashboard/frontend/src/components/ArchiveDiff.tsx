import { useEffect, useRef, useState } from 'react'
import {
  createChart,
  AreaSeries,
  ColorType,
  CrosshairMode,
  type IChartApi,
  type UTCTimestamp,
} from 'lightweight-charts'

// Diff view: pick two runs by name, render their summary side-by-side
// with a delta column, plus the two equity curves stacked vertically.
// Reached via location.hash:
//   /archive#diff:<runA>:<runB>
// where the run names are the raw on-disk dir names (URL-encoded).

interface MarkoutHorizon {
  resolved_fills: number
  avg_bps: number
  avg_buy_bps: number
  avg_sell_bps: number
}

interface Summary {
  starting_capital: number
  final_equity: number
  total_pnl: number
  return_pct: number
  max_drawdown_pct: number
  sharpe_per_fill: number
  total_fills: number
  win_fills: number
  win_rate_pct: number
  buy_count?: number
  sell_count?: number
  buy_notional_usd?: number
  sell_notional_usd?: number
  simulation_start?: string
  simulation_end?: string
  wallclock_duration_ms?: number
  instruments?: string[]
  strategy_name?: string
  params_hash?: string
  git_sha?: string
  markouts?: {
    '50ms'?: MarkoutHorizon
    '1s'?: MarkoutHorizon
    '5s'?: MarkoutHorizon
    '30s'?: MarkoutHorizon
  }
  round_trips?: {
    closed_round_trips: number
    avg_holding_ms?: number
    median_holding_ms?: number
    round_trip_win_rate_pct?: number
    avg_round_trip_pnl?: number
  }
}

interface RunDetail {
  name: string
  summary: Summary | null
  trades: unknown[]
  pnlCurve: Array<{ ts: number; equity: number }>
}

interface Props {
  runA: string
  runB: string
}

function fmt(value: number | undefined, digits = 2, suffix = ''): string {
  if (value === undefined || Number.isNaN(value)) return '—'
  return `${value.toFixed(digits)}${suffix}`
}

function deltaClass(delta: number, higherIsBetter = true): string {
  if (Math.abs(delta) < 1e-9) return 'pnl-zero'
  const positive = delta > 0
  return positive === higherIsBetter ? 'pnl-pos' : 'pnl-neg'
}

function deltaStr(delta: number, digits = 2, suffix = ''): string {
  const sign = delta >= 0 ? '+' : ''
  return `${sign}${delta.toFixed(digits)}${suffix}`
}

interface MetricRow {
  label: string
  a: number | undefined
  b: number | undefined
  digits: number
  suffix?: string
  higherIsBetter: boolean
}

export function ArchiveDiff({ runA, runB }: Props) {
  const [a, setA] = useState<RunDetail | null>(null)
  const [b, setB] = useState<RunDetail | null>(null)
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    setA(null)
    setB(null)
    setError(null)
    Promise.all([
      fetch(`/api/backtest-runs/${encodeURIComponent(runA)}`).then((r) => r.json()),
      fetch(`/api/backtest-runs/${encodeURIComponent(runB)}`).then((r) => r.json()),
    ])
      .then(([dA, dB]: [RunDetail, RunDetail]) => {
        setA(dA)
        setB(dB)
      })
      .catch((e) => setError(String(e)))
  }, [runA, runB])

  if (error)
    return (
      <div className="archive-body" style={{ padding: 16, color: 'var(--red)' }}>
        Error: {error}
      </div>
    )
  if (!a || !b)
    return (
      <div className="archive-body" style={{ padding: 16, color: 'var(--text-muted)' }}>
        Loading…
      </div>
    )
  if (!a.summary || !b.summary)
    return (
      <div className="archive-body" style={{ padding: 16, color: 'var(--red)' }}>
        Missing summary.json
      </div>
    )

  const sa = a.summary
  const sb = b.summary

  const rows: MetricRow[] = [
    { label: 'Total PnL ($)', a: sa.total_pnl, b: sb.total_pnl, digits: 4, higherIsBetter: true },
    {
      label: 'Return %',
      a: sa.return_pct * 100,
      b: sb.return_pct * 100,
      digits: 4,
      suffix: '%',
      higherIsBetter: true,
    },
    {
      label: 'Max DD %',
      a: sa.max_drawdown_pct,
      b: sb.max_drawdown_pct,
      digits: 4,
      suffix: '%',
      higherIsBetter: false,
    },
    {
      label: 'Sharpe / fill',
      a: sa.sharpe_per_fill,
      b: sb.sharpe_per_fill,
      digits: 4,
      higherIsBetter: true,
    },
    {
      label: 'Win rate %',
      a: sa.win_rate_pct,
      b: sb.win_rate_pct,
      digits: 2,
      suffix: '%',
      higherIsBetter: true,
    },
    { label: 'Total fills', a: sa.total_fills, b: sb.total_fills, digits: 0, higherIsBetter: true },
    { label: 'Buys', a: sa.buy_count, b: sb.buy_count, digits: 0, higherIsBetter: true },
    { label: 'Sells', a: sa.sell_count, b: sb.sell_count, digits: 0, higherIsBetter: true },
    {
      label: 'Buy notional ($)',
      a: sa.buy_notional_usd,
      b: sb.buy_notional_usd,
      digits: 2,
      higherIsBetter: true,
    },
    {
      label: 'Sell notional ($)',
      a: sa.sell_notional_usd,
      b: sb.sell_notional_usd,
      digits: 2,
      higherIsBetter: true,
    },
    {
      label: 'Wallclock (s)',
      a: sa.wallclock_duration_ms !== undefined ? sa.wallclock_duration_ms / 1000 : undefined,
      b: sb.wallclock_duration_ms !== undefined ? sb.wallclock_duration_ms / 1000 : undefined,
      digits: 2,
      suffix: 's',
      higherIsBetter: false,
    },
  ]

  // Markout rows — only if either run has them. Higher = better
  // (positive markout means price moved with you, less adverse).
  const horizons: Array<'50ms' | '1s' | '5s' | '30s'> = ['50ms', '1s', '5s', '30s']
  const markoutRows: MetricRow[] = []
  if (sa.markouts || sb.markouts) {
    for (const h of horizons) {
      markoutRows.push({
        label: `Markout ${h} (bps)`,
        a: sa.markouts?.[h]?.avg_bps,
        b: sb.markouts?.[h]?.avg_bps,
        digits: 2,
        higherIsBetter: true,
      })
    }
  }

  // Round-trip rows — strategy-agnostic, so always include if either
  // run has them. "higherIsBetter" varies: more closed trips & higher
  // win-rate are good; longer holding period is neither — we set false
  // (shorter = "better" for risk turnover) but it's mostly informative.
  const rtRows: MetricRow[] = []
  if (sa.round_trips || sb.round_trips) {
    rtRows.push(
      {
        label: 'Closed round-trips',
        a: sa.round_trips?.closed_round_trips,
        b: sb.round_trips?.closed_round_trips,
        digits: 0,
        higherIsBetter: true,
      },
      {
        label: 'Avg holding (ms)',
        a: sa.round_trips?.avg_holding_ms,
        b: sb.round_trips?.avg_holding_ms,
        digits: 0,
        higherIsBetter: false,
      },
      {
        label: 'Median holding (ms)',
        a: sa.round_trips?.median_holding_ms,
        b: sb.round_trips?.median_holding_ms,
        digits: 0,
        higherIsBetter: false,
      },
      {
        label: 'RT win rate %',
        a: sa.round_trips?.round_trip_win_rate_pct,
        b: sb.round_trips?.round_trip_win_rate_pct,
        digits: 2,
        suffix: '%',
        higherIsBetter: true,
      },
      {
        label: 'Avg RT PnL ($)',
        a: sa.round_trips?.avg_round_trip_pnl,
        b: sb.round_trips?.avg_round_trip_pnl,
        digits: 4,
        higherIsBetter: true,
      }
    )
  }

  return (
    <div className="archive-body archive-body--diff">
      <div className="panel" style={{ overflow: 'auto' }}>
        <div className="panel-header">
          <span className="panel-title">Run diff</span>
          <span className="panel-badge" style={{ color: 'var(--text-muted)' }}>
            {sa.strategy_name ?? '—'} {sa.git_sha ? sa.git_sha.slice(0, 7) : ''}{' '}
            {sa.params_hash ? sa.params_hash.slice(0, 8) : ''}
            {' vs '}
            {sb.strategy_name ?? '—'} {sb.git_sha ? sb.git_sha.slice(0, 7) : ''}{' '}
            {sb.params_hash ? sb.params_hash.slice(0, 8) : ''}
          </span>
        </div>
        <div style={{ padding: 12, overflow: 'auto' }}>
          <table className="blotter-table" style={{ width: '100%' }}>
            <thead>
              <tr>
                <th>Metric</th>
                <th className="num" style={{ color: 'var(--blue)' }}>
                  A: {a.name}
                </th>
                <th className="num" style={{ color: 'var(--yellow)' }}>
                  B: {b.name}
                </th>
                <th className="num">Δ (B − A)</th>
              </tr>
            </thead>
            <tbody>
              {[...rows, ...markoutRows, ...rtRows].map((r, idx) => {
                const av = r.a
                const bv = r.b
                const delta = av !== undefined && bv !== undefined ? bv - av : undefined
                // Visual break before each metric block.
                const isMarkoutStart = idx === rows.length && markoutRows.length > 0
                const isRtStart = idx === rows.length + markoutRows.length && rtRows.length > 0
                const sectionTop = isMarkoutStart || isRtStart
                return (
                  <tr
                    key={r.label}
                    style={sectionTop ? { borderTop: '2px solid var(--border-strong)' } : undefined}
                  >
                    <td>{r.label}</td>
                    <td className="num">{fmt(av, r.digits, r.suffix)}</td>
                    <td className="num">{fmt(bv, r.digits, r.suffix)}</td>
                    <td
                      className={`num ${delta !== undefined ? deltaClass(delta, r.higherIsBetter) : ''}`}
                    >
                      {delta !== undefined ? deltaStr(delta, r.digits, r.suffix) : '—'}
                    </td>
                  </tr>
                )
              })}
            </tbody>
          </table>

          <h3
            style={{
              marginTop: 24,
              marginBottom: 8,
              fontSize: 11,
              letterSpacing: '0.09em',
              color: 'var(--text-label)',
              textTransform: 'uppercase',
            }}
          >
            Equity curves
          </h3>
          <DualEquity
            seriesA={a.pnlCurve
              .filter((p) => p.ts > 0)
              .map((p) => ({ ts: p.ts, equity: p.equity }))}
            seriesB={b.pnlCurve
              .filter((p) => p.ts > 0)
              .map((p) => ({ ts: p.ts, equity: p.equity }))}
            labelA={a.name}
            labelB={b.name}
          />
        </div>
      </div>
    </div>
  )
}

// Standalone overlay chart — keeps the existing single-series EquityChart
// untouched so live + archive detail keep their behaviour. Uses
// lightweight-charts directly because the prop-injection seam in
// EquityChart only carries one fills array.

interface DualEquityProps {
  seriesA: Array<{ ts: number; equity: number }>
  seriesB: Array<{ ts: number; equity: number }>
  labelA: string
  labelB: string
}

function DualEquity({ seriesA, seriesB }: DualEquityProps) {
  const hostRef = useRef<HTMLDivElement>(null)
  const chartRef = useRef<IChartApi | null>(null)

  useEffect(() => {
    if (!hostRef.current) return
    const chart = createChart(hostRef.current, {
      layout: {
        background: { type: ColorType.Solid, color: '#0d1117' },
        textColor: '#768390',
        fontFamily: '"SF Mono", "Fira Code", Menlo, monospace',
        fontSize: 11,
      },
      grid: {
        vertLines: { color: '#141b26' },
        horzLines: { color: '#141b26' },
      },
      rightPriceScale: { borderColor: '#1c2333' },
      timeScale: { borderColor: '#1c2333', timeVisible: true, secondsVisible: false },
      crosshair: {
        mode: CrosshairMode.Normal,
        vertLine: { color: '#243044', width: 1, style: 3 },
        horzLine: { color: '#243044', width: 1, style: 3 },
      },
      height: 260,
    })
    const aSeries = chart.addSeries(AreaSeries, {
      lineColor: '#388bfd',
      topColor: 'rgba(56,139,253,0.28)',
      bottomColor: 'rgba(56,139,253,0)',
      lineWidth: 2,
      priceLineVisible: false,
    })
    const bSeries = chart.addSeries(AreaSeries, {
      lineColor: '#d29922',
      topColor: 'rgba(210,153,34,0.20)',
      bottomColor: 'rgba(210,153,34,0)',
      lineWidth: 2,
      priceLineVisible: false,
    })
    chartRef.current = chart

    const fold = (raw: Array<{ ts: number; equity: number }>) => {
      const m = new Map<number, number>()
      for (const p of raw) m.set(Math.floor(p.ts / 1_000_000_000), p.equity)
      return [...m.entries()]
        .sort(([a], [b]) => a - b)
        .map(([t, v]) => ({ time: t as UTCTimestamp, value: v }))
    }
    aSeries.setData(fold(seriesA))
    bSeries.setData(fold(seriesB))
    chart.timeScale().fitContent()

    const ro = new ResizeObserver((entries) => {
      for (const e of entries) chart.resize(e.contentRect.width, 260)
    })
    ro.observe(hostRef.current)
    return () => {
      ro.disconnect()
      chart.remove()
      chartRef.current = null
    }
  }, [seriesA, seriesB])

  return <div ref={hostRef} style={{ width: '100%', height: 260 }} />
}
