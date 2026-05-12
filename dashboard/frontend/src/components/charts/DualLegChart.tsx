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
import { useStore } from '../../store'

// Mainchart for FundingArb-style strategies. Plots both legs (spot in
// blue, perp in orange) on the same axis so the visual gap matches the
// basis-bps number in the strategy-state panel. Fed by strategyState
// frames (2 Hz), not by md_data ticks — the bridge labels every BBO
// with one symbol, so going through the tick stream would aggregate
// both legs into a single line. Driving from the state JSON keeps the
// two legs distinct.

const CHART_THEME = {
  bg: '#0d1117',
  text: '#768390',
  grid: '#141b26',
  border: '#1c2333',
  crosshair: '#243044',
  spot: '#5c9ce6',
  perp: '#e89436',
}

// 30 minutes at 2 Hz. Bigger than the in-panel version (5 min) since
// this chart owns the full main-row slot.
const MAX_POINTS = 3600

// Pick a price-axis precision that fits the magnitude. lightweight-
// charts defaults to 2 dp (good for stocks); FundingArb commonly runs
// on long-tail tokens priced under $1 (PURR ~$0.07), where 2 dp
// flattens the entire chart to one horizontal line.
function precisionFor(price: number): { precision: number; minMove: number } {
    if (price <= 0) return { precision: 4, minMove: 0.0001 }
    if (price >= 1000) return { precision: 2, minMove: 0.01 }
    if (price >= 1) return { precision: 4, minMove: 0.0001 }
    if (price >= 0.01) return { precision: 6, minMove: 0.000001 }
    return { precision: 8, minMove: 0.00000001 }
}

export function DualLegChart() {
  const strategyState = useStore((s) => s.strategyState)
  // Only render leg history when the active strategy is funding-arb
  // shaped. Anything else feeds zero into the series and would skew
  // the price scale.
  const fa = strategyState?.kind === 'FundingArb' ? strategyState : null

  const hostRef = useRef<HTMLDivElement>(null)
  const chartRef = useRef<IChartApi | null>(null)
  const spotRef = useRef<ISeriesApi<'Line'> | null>(null)
  const perpRef = useRef<ISeriesApi<'Line'> | null>(null)
  const lastTsRef = useRef<number>(0)
  const pointCountRef = useRef<number>(0)
  const precisionSetRef = useRef<boolean>(false)

  useEffect(() => {
    if (!hostRef.current) return
    const chart = createChart(hostRef.current, {
      layout: {
        background: { type: ColorType.Solid, color: CHART_THEME.bg },
        textColor: CHART_THEME.text,
        fontFamily: '"SF Mono", "Fira Code", Menlo, monospace',
        fontSize: 11,
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
    // Default to 6 dp — FundingArb is typically used on long-tail tokens
    // priced under $1 (PURR ~$0.07, with basis ~$0.0002). The data-update
    // effect below upgrades precision once the first spot frame arrives,
    // but starting at 6 dp avoids the brief window where lightweight-
    // charts' default of 2 dp would flatten both lines onto each other.
    const initialFmt = { type: 'price' as const, precision: 6, minMove: 0.000001 }
    spotRef.current = chart.addSeries(LineSeries, {
      color: CHART_THEME.spot,
      lineWidth: 2,
      title: 'spot',
      priceFormat: initialFmt,
    })
    perpRef.current = chart.addSeries(LineSeries, {
      color: CHART_THEME.perp,
      lineWidth: 2,
      title: 'perp',
      priceFormat: initialFmt,
    })
    chartRef.current = chart

    const ro = new ResizeObserver(() => {
      if (hostRef.current && chartRef.current) {
        chartRef.current.applyOptions({
          width: hostRef.current.clientWidth,
          height: hostRef.current.clientHeight,
        })
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

  useEffect(() => {
    if (!fa) return
    const spot = spotRef.current
    const perp = perpRef.current
    if (!spot || !perp) return

    // First-frame: derive price-axis precision from the spot mid so
    // the chart shows enough decimals for low-priced tokens (PURR at
    // $0.07 needs 6 dp to surface the basis). Set once on each series
    // — lightweight-charts honours the format for the whole y-axis.
    if (!precisionSetRef.current && fa.spotPx > 0) {
      const fmt = precisionFor(fa.spotPx)
      spot.applyOptions({ priceFormat: { type: 'price', ...fmt } })
      perp.applyOptions({ priceFormat: { type: 'price', ...fmt } })
      precisionSetRef.current = true
    }

    let ts = Math.floor(Date.now() / 1000)
    if (ts <= lastTsRef.current) ts = lastTsRef.current + 1
    lastTsRef.current = ts
    const t = ts as UTCTimestamp
    if (fa.spotPx > 0) spot.update({ time: t, value: fa.spotPx })
    if (fa.perpPx > 0) perp.update({ time: t, value: fa.perpPx })
    pointCountRef.current += 1
    // Auto-fit only on the first MAX_POINTS frames so the operator can
    // pan/zoom freely afterwards without the view snapping back.
    if (pointCountRef.current < MAX_POINTS && chartRef.current) {
      chartRef.current.timeScale().fitContent()
    }
  }, [fa, fa?.spotPx, fa?.perpPx])

  return (
    <div className="panel">
      <div className="panel-header">
        <span className="panel-title">
          {fa ? `${fa.symbol} · spot vs perp` : 'spot vs perp'}
        </span>
        <span className="panel-badge">
          {fa
            ? `basis ${fa.basisBps >= 0 ? '+' : ''}${fa.basisBps.toFixed(1)} bps`
            : '—'}
        </span>
      </div>
      <div ref={hostRef} style={{ width: '100%', height: '100%', flex: 1 }} />
    </div>
  )
}
