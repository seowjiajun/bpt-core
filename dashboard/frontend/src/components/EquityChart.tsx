import { useEffect, useRef } from 'react'
import {
  createChart,
  AreaSeries,
  ColorType,
  CrosshairMode,
  type IChartApi,
  type ISeriesApi,
  type UTCTimestamp,
} from 'lightweight-charts'
import { useStore } from '../store'
import type { Fill } from './Blotter'

interface EquityChartProps {
  // Optional overrides — when provided, the chart renders these instead of
  // the live store.  Used by the backtest archive view.
  fills?: Array<Pick<Fill, 'ts' | 'equity'>>
  startingCapital?: number
}

const CHART_THEME = {
  bg:        '#0d1117',
  text:      '#768390',
  grid:      '#141b26',
  border:    '#1c2333',
  line:      '#388bfd',
  fillTop:   'rgba(56, 139, 253, 0.28)',
  fillBot:   'rgba(56, 139, 253, 0.00)',
  crosshair: '#243044',
}

export function EquityChart(props: EquityChartProps = {}) {
  const liveFills = useStore((s) => s.fills)
  const liveCap = useStore((s) => s.startingCapital)
  const fills = props.fills ?? liveFills
  const startingCapital = props.startingCapital ?? liveCap
  const hostRef = useRef<HTMLDivElement>(null)
  const chartRef = useRef<IChartApi | null>(null)
  const seriesRef = useRef<ISeriesApi<'Area'> | null>(null)

  useEffect(() => {
    if (!hostRef.current) return

    const chart = createChart(hostRef.current, {
      layout: {
        background: { type: ColorType.Solid, color: CHART_THEME.bg },
        textColor:  CHART_THEME.text,
        fontFamily: '"SF Mono", "Fira Code", Menlo, monospace',
        fontSize:   11,
      },
      grid: {
        vertLines: { color: CHART_THEME.grid },
        horzLines: { color: CHART_THEME.grid },
      },
      rightPriceScale: { borderColor: CHART_THEME.border },
      timeScale: {
        borderColor: CHART_THEME.border,
        timeVisible: true,
        secondsVisible: false,
      },
      crosshair: {
        mode: CrosshairMode.Normal,
        vertLine: { color: CHART_THEME.crosshair, width: 1, style: 3 },
        horzLine: { color: CHART_THEME.crosshair, width: 1, style: 3 },
      },
    })

    const series = chart.addSeries(AreaSeries, {
      lineColor:         CHART_THEME.line,
      topColor:          CHART_THEME.fillTop,
      bottomColor:       CHART_THEME.fillBot,
      lineWidth:         2,
      priceLineVisible:  false,
      lastValueVisible:  true,
    })

    chartRef.current = chart
    seriesRef.current = series

    const ro = new ResizeObserver(entries => {
      for (const e of entries) {
        chart.resize(e.contentRect.width, e.contentRect.height)
      }
    })
    ro.observe(hostRef.current)

    return () => {
      ro.disconnect()
      chart.remove()
      chartRef.current = null
      seriesRef.current = null
    }
  }, [])

  useEffect(() => {
    const series = seriesRef.current
    if (!series) return

    // Anchor point at the starting capital just before the first fill so the
    // line starts flat at $100k and visibly responds to the first trade.
    const firstTs = fills.length ? Math.floor(fills[0].ts / 1_000_000_000) - 60 : 0
    const points = [
      { time: firstTs as UTCTimestamp, value: startingCapital },
      ...fills.map(f => ({
        time: Math.floor(f.ts / 1_000_000_000) as UTCTimestamp,
        value: f.equity,
      })),
    ]

    series.setData(points)
    chartRef.current?.timeScale().fitContent()
  }, [fills, startingCapital])

  return <div className="chart-host" ref={hostRef} />
}
