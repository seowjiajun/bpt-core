import { useEffect, useRef } from 'react'
import {
  createChart,
  LineSeries,
  ColorType,
  CrosshairMode,
  type IChartApi,
  type ISeriesApi,
  type UTCTimestamp,
} from 'lightweight-charts'
import type { FundingArbStrategyState } from '../../types/messages'

const fmt = (v: number, d = 2) => v.toFixed(d)
const fmtSigned = (v: number, d = 6) => (v >= 0 ? '+' : '') + v.toFixed(d)
const fmtBps = (v: number) => v.toFixed(1)
const fmtPct = (v: number) => (v * 100).toFixed(3) + '%'

const LEG_LABEL: Record<FundingArbStrategyState['legStatus'], string> = {
  flat: 'FLAT',
  entering: 'ENTERING',
  open: 'OPEN',
  exiting: 'EXITING',
}

const LEG_COLOUR: Record<FundingArbStrategyState['legStatus'], string> = {
  flat: 'stat-value--muted',
  entering: 'limit-warn',
  open: 'stat-value--green',
  exiting: 'limit-warn',
}

// Per-panel chart palette. Spot/perp diverge for the same base asset
// by basis bps; on a single y-axis they sit on top of each other when
// basis is small, so the two colours need to read clearly even when
// the lines overlap pixel-for-pixel.
const CHART_THEME = {
  bg: '#0d1117',
  text: '#768390',
  grid: '#141b26',
  border: '#1c2333',
  crosshair: '#243044',
  spot: '#5c9ce6',
  perp: '#e89436',
}

// Keep the in-memory chart series bounded; the panel state arrives at
// 2 Hz, so 600 points ≈ 5 minutes of history.
const MAX_POINTS = 600

export function FundingArbPanel({ state: ss }: { state: FundingArbStrategyState }) {
  const deltaClass =
    Math.abs(ss.hedgedDelta) < 1e-4
      ? 'stat-value--green'
      : Math.abs(ss.hedgedDelta) < 1e-2
        ? 'limit-warn'
        : 'stat-value--red'

  // ── Mini dual-leg chart ───────────────────────────────────────────────
  // The shared PriceChart at the top of the dashboard plots a single
  // instrument's candles (the bridge labels every BBO with one symbol).
  // For funding arb the interesting view is the *gap* between the two
  // legs; render a per-panel chart driven by strategyState frames so
  // operators can see basis dynamics without a second WS hookup.
  const hostRef = useRef<HTMLDivElement>(null)
  const chartRef = useRef<IChartApi | null>(null)
  const spotRef = useRef<ISeriesApi<'Line'> | null>(null)
  const perpRef = useRef<ISeriesApi<'Line'> | null>(null)
  const lastTsRef = useRef<number>(0)
  const pointCountRef = useRef<number>(0)

  useEffect(() => {
    if (!hostRef.current) return
    const chart = createChart(hostRef.current, {
      layout: {
        background: { type: ColorType.Solid, color: CHART_THEME.bg },
        textColor: CHART_THEME.text,
        fontFamily: '"SF Mono", "Fira Code", Menlo, monospace',
        fontSize: 10,
      },
      grid: {
        vertLines: { color: CHART_THEME.grid },
        horzLines: { color: CHART_THEME.grid },
      },
      rightPriceScale: { borderColor: CHART_THEME.border },
      timeScale: {
        borderColor: CHART_THEME.border,
        timeVisible: true,
        secondsVisible: true,
      },
      crosshair: {
        mode: CrosshairMode.Normal,
        vertLine: { color: CHART_THEME.crosshair, width: 1, style: 3 },
        horzLine: { color: CHART_THEME.crosshair, width: 1, style: 3 },
      },
    })
    spotRef.current = chart.addSeries(LineSeries, {
      color: CHART_THEME.spot,
      lineWidth: 2,
      title: 'spot',
    })
    perpRef.current = chart.addSeries(LineSeries, {
      color: CHART_THEME.perp,
      lineWidth: 2,
      title: 'perp',
    })
    chartRef.current = chart

    // Resize chart to container width on layout changes (the panel sits
    // in a CSS grid cell whose width tracks the viewport).
    const ro = new ResizeObserver(() => {
      if (hostRef.current && chartRef.current) {
        chartRef.current.applyOptions({ width: hostRef.current.clientWidth })
      }
    })
    ro.observe(hostRef.current)

    return () => {
      ro.disconnect()
      chart.remove()
      chartRef.current = null
      spotRef.current = null
      perpRef.current = null
    }
  }, [])

  // Append a sample on every state update. lightweight-charts wants
  // monotonically non-decreasing timestamps — clamp to last+1 if the
  // wall clock duplicates a second (state arrives at 2 Hz so this fires
  // every other frame). Only push when the leg has a real mid; zero
  // (testnet one-sided BBOs filtered by MdValidator) would drag the
  // line off-scale.
  useEffect(() => {
    const spot = spotRef.current
    const perp = perpRef.current
    if (!spot || !perp) return
    let ts = Math.floor(Date.now() / 1000)
    if (ts <= lastTsRef.current) ts = lastTsRef.current + 1
    lastTsRef.current = ts
    const t = ts as UTCTimestamp
    if (ss.spotPx > 0) spot.update({ time: t, value: ss.spotPx })
    if (ss.perpPx > 0) perp.update({ time: t, value: ss.perpPx })
    pointCountRef.current += 1
    // Auto-fit only on the first ~MAX_POINTS frames so the operator can
    // pan/zoom freely afterwards without the view snapping back.
    if (pointCountRef.current < MAX_POINTS && chartRef.current) {
      chartRef.current.timeScale().fitContent()
    }
  }, [ss.spotPx, ss.perpPx])

  return (
    <div className="panel" style={{ gridArea: 'stratstate' }}>
      <div className="panel-header">
        <span className="panel-title">Strategy State</span>
        <span className="panel-badge">
          FundingArb · <span className={LEG_COLOUR[ss.legStatus]}>{LEG_LABEL[ss.legStatus]}</span>
        </span>
      </div>

      <table className="blotter-table" style={{ fontSize: 11 }}>
        <thead>
          <tr>
            <th colSpan={2} style={{ textAlign: 'left' }}>
              Legs
            </th>
            <th colSpan={2} style={{ textAlign: 'left' }}>
              Signal
            </th>
          </tr>
        </thead>
        <tbody>
          <tr>
            <td style={{ color: CHART_THEME.spot, width: 55 }}>SPOT</td>
            <td className="num">
              {fmt(ss.spotPx, 4)} <span className="stat-value--muted">× {fmtSigned(ss.spotQty)}</span>
            </td>
            <td style={{ color: 'var(--text-muted)', width: 60 }}>FUND</td>
            <td className="num">{fmtPct(ss.fundingRate)}</td>
          </tr>
          <tr>
            <td style={{ color: CHART_THEME.perp }}>PERP</td>
            <td className="num">
              {fmt(ss.perpPx, 4)} <span className="stat-value--muted">× {fmtSigned(ss.perpQty)}</span>
            </td>
            <td style={{ color: 'var(--text-muted)' }}>FUND-APR</td>
            <td className="num">{fmt(ss.fundingApr * 100, 2)}%</td>
          </tr>
          <tr>
            <td style={{ color: 'var(--text-muted)' }}>BASIS</td>
            <td
              className={`num ${ss.basisBps > 0.5 ? 'stat-value--green' : ss.basisBps < -0.5 ? 'stat-value--red' : 'stat-value--muted'}`}
            >
              {fmtBps(ss.basisBps)} bps
            </td>
            <td style={{ color: 'var(--text-muted)' }}>DELTA</td>
            <td className={`num ${deltaClass}`}>{fmtSigned(ss.hedgedDelta)}</td>
          </tr>
        </tbody>
      </table>

      <div ref={hostRef} style={{ width: '100%', height: 140 }} />
    </div>
  )
}
