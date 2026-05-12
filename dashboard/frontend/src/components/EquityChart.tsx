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
  bg: '#0d1117',
  text: '#768390',
  grid: '#141b26',
  border: '#1c2333',
  line: '#388bfd',
  fillTop: 'rgba(56, 139, 253, 0.28)',
  fillBot: 'rgba(56, 139, 253, 0.00)',
  crosshair: '#243044',
}

export function EquityChart(props: EquityChartProps = {}) {
  // Two modes:
  //   1. Live/paper — driven by bpt-order-gateway AccountSnapshots in the store.
  //      props.fills is undefined; we plot accountHistory directly.
  //   2. Archive — caller passes `fills` (with absolute equity values from
  //      summary.json) and a startingCapital anchor. No store access.
  const accountHistory = useStore((s) => s.accountHistory)
  const liveFills = useStore((s) => s.fills)
  const isArchive = props.fills !== undefined
  const fills = props.fills ?? []
  const startingCapital = props.startingCapital ?? 0
  const hostRef = useRef<HTMLDivElement>(null)
  const chartRef = useRef<IChartApi | null>(null)
  const seriesRef = useRef<ISeriesApi<'Area'> | null>(null)

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
        secondsVisible: false,
      },
      crosshair: {
        mode: CrosshairMode.Normal,
        vertLine: { color: CHART_THEME.crosshair, width: 1, style: 3 },
        horzLine: { color: CHART_THEME.crosshair, width: 1, style: 3 },
      },
    })

    const series = chart.addSeries(AreaSeries, {
      lineColor: CHART_THEME.line,
      topColor: CHART_THEME.fillTop,
      bottomColor: CHART_THEME.fillBot,
      lineWidth: 2,
      priceLineVisible: false,
      lastValueVisible: true,
    })

    chartRef.current = chart
    seriesRef.current = series

    const ro = new ResizeObserver((entries) => {
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

    // Lightweight-charts requires strictly ascending timestamps, so dedupe by
    // second-granularity time and keep the latest value per second.
    const byTime = new Map<number, number>()
    if (isArchive) {
      for (const f of fills) {
        byTime.set(Math.floor(f.ts / 1_000_000_000), f.equity)
      }
    } else {
      // Live mode — build a deposit-immune equity curve: anchor on the first
      // observed account equity, then add cumulative realizedPnl from each
      // fill. Raw accountHistory.equity is contaminated by spot↔perp transfers
      // (HL unified mode auto-collateralizes when AS needs perp margin), so a
      // raw plot jumps on every transfer and looks like P&L when it isn't.
      // realizedPnl per fill is the true trading P&L; cumulative sum is the
      // trading-only equity curve. Same root fix as RiskPanel live mode.
      //
      // Caveat (matches RiskPanel Max DD): unrealized PnL between fills isn't
      // included, so intra-fill swings are invisible. Acceptable at AS-style
      // fill cadence; revisit if fills become sparse.
      const anchorEquity = accountHistory[0]?.equity ?? 0
      if (accountHistory.length > 0) {
        byTime.set(Math.floor(accountHistory[0].ts / 1_000_000_000), anchorEquity)
      }
      const sortedFills = [...liveFills].sort((a, b) => a.ts - b.ts)
      let runningPnl = 0
      for (const f of sortedFills) {
        runningPnl += f.realizedPnl
        byTime.set(Math.floor(f.ts / 1_000_000_000), anchorEquity + runningPnl)
      }
    }

    // Anchor point just before the first event so the line starts flat
    // and visibly responds to the first change. In archive mode the
    // anchor is the run's starting capital; in live mode it's the first
    // observed account equity (no static capital baseline exists).
    const sorted = [...byTime.entries()].sort(([a], [b]) => a - b)
    const points: Array<{ time: UTCTimestamp; value: number }> = []
    if (sorted.length > 0) {
      const anchorTime = (sorted[0][0] - 60) as UTCTimestamp
      const anchorVal = isArchive ? startingCapital : sorted[0][1]
      points.push({ time: anchorTime, value: anchorVal })
    }
    for (const [t, v] of sorted) {
      points.push({ time: t as UTCTimestamp, value: v })
    }

    series.setData(points)
    chartRef.current?.timeScale().fitContent()
  }, [fills, startingCapital, accountHistory, liveFills, isArchive])

  return <div className="chart-host" ref={hostRef} />
}
