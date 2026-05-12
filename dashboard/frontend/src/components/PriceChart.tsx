import { useEffect, useRef } from 'react'
import {
  createChart,
  CandlestickSeries,
  createSeriesMarkers,
  ColorType,
  CrosshairMode,
  LineStyle,
  type IChartApi,
  type IPriceLine,
  type ISeriesApi,
  type ISeriesMarkersPluginApi,
  type SeriesMarker,
  type Time,
  type UTCTimestamp,
} from 'lightweight-charts'
import { useStore } from '../store'

const CHART_THEME = {
  bg: '#0d1117',
  text: '#768390',
  grid: '#141b26',
  border: '#1c2333',
  up: '#3fb950',
  down: '#f85149',
  crosshair: '#243044',
  // Overlay palette for AS quote placement. Market bid/ask sit behind
  // our quotes as dim reference rails; our quotes use the same up/down
  // hues but brighter. Reservation is an off-axis yellow so it reads
  // as "AS intent" distinct from actual order levels.
  marketBid: '#1e4a2a',
  marketAsk: '#4a1e22',
  ourBid: '#3fb950',
  ourAsk: '#f85149',
  reservation: '#d4a72c',
  suppressed: '#5a5a5a',
}

export function PriceChart() {
  const fills = useStore((s) => s.fills)
  const candles = useStore((s) => s.candles)
  const strat = useStore((s) => s.strategyState)
  const hostRef = useRef<HTMLDivElement>(null)
  const chartRef = useRef<IChartApi | null>(null)
  const seriesRef = useRef<ISeriesApi<'Candlestick'> | null>(null)
  const markersRef = useRef<ISeriesMarkersPluginApi<Time> | null>(null)
  const didFitRef = useRef(false)

  // Price-line refs: one per overlay level. Null when a price isn't
  // valid (book unwarmed / order not live); lines are created lazily and
  // removed when invalidated to avoid "phantom 0 lines at the bottom".
  const marketBidLineRef = useRef<IPriceLine | null>(null)
  const marketAskLineRef = useRef<IPriceLine | null>(null)
  const ourBidLineRef = useRef<IPriceLine | null>(null)
  const ourAskLineRef = useRef<IPriceLine | null>(null)
  const reservationLineRef = useRef<IPriceLine | null>(null)

  // Create chart + set static candles once.
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

    const series = chart.addSeries(CandlestickSeries, {
      upColor: CHART_THEME.up,
      downColor: CHART_THEME.down,
      borderUpColor: CHART_THEME.up,
      borderDownColor: CHART_THEME.down,
      wickUpColor: CHART_THEME.up,
      wickDownColor: CHART_THEME.down,
    })

    chartRef.current = chart
    seriesRef.current = series
    markersRef.current = createSeriesMarkers(series, [])

    const ro = new ResizeObserver((entries) => {
      for (const e of entries) chart.resize(e.contentRect.width, e.contentRect.height)
    })
    ro.observe(hostRef.current)

    return () => {
      ro.disconnect()
      chart.remove()
      chartRef.current = null
      seriesRef.current = null
      markersRef.current = null
      didFitRef.current = false
    }
  }, [])

  // Stream candles as they update. Using series.update(latestBar) — NOT
  // setData — is critical: setData replaces the full series on every tick
  // and caused visible lag. series.update is O(1): if the bar's time
  // matches the current last bar it updates in place, otherwise it
  // appends. That handles both "tick inside current bucket" and
  // "tick crossed into a new bucket" cases.
  useEffect(() => {
    const series = seriesRef.current
    if (!series || candles.length === 0) return
    const last = candles[candles.length - 1]
    series.update({
      time: last.time as UTCTimestamp,
      open: last.open,
      high: last.high,
      low: last.low,
      close: last.close,
    })
    if (!didFitRef.current && candles.length >= 5) {
      chartRef.current?.timeScale().fitContent()
      didFitRef.current = true
    }
  }, [candles])

  // Stream markers as fills arrive — no chart reload, just updating the marker layer.
  useEffect(() => {
    const markers = markersRef.current
    if (!markers) return
    const data: SeriesMarker<Time>[] = fills.map((f) => ({
      time: Math.floor(f.ts / 1_000_000_000) as UTCTimestamp,
      position: f.side === 'BUY' ? 'belowBar' : 'aboveBar',
      color: f.side === 'BUY' ? CHART_THEME.up : CHART_THEME.down,
      shape: f.side === 'BUY' ? 'arrowUp' : 'arrowDown',
      text: `${f.side[0]} ${f.qty}@${f.price.toFixed(0)}`,
    }))
    markers.setMarkers(data)
  }, [fills])

  // Price-line overlay: market bid/ask + our bid/ask + reservation.
  //
  // Keeping these as priceLine (not a line series) is deliberate —
  // they're instantaneous horizontals that move with the quoter, not
  // time-varying series. createPriceLine returns a handle we mutate via
  // applyOptions() for live updates, or remove when the price becomes
  // invalid (book unwarmed → marketBid=0; order not live → hide).
  //
  // Suppression styling: when bidSuppressed/askSuppressed is true, the
  // line stays visible but greys + dashes so the dashboard reflects
  // that AS is deliberately quiet on that side (drift/tyr/inv/vol/queue
  // reason shown in StrategyStatePanel). Disappearing would look like
  // a plumbing bug.
  useEffect(() => {
    const series = seriesRef.current
    if (!series) return

    type LineSpec = {
      price: number | undefined
      color: string
      style: LineStyle
      title: string
      lineWidth: 1 | 2
    }

    const sync = (ref: React.MutableRefObject<IPriceLine | null>, spec: LineSpec) => {
      const valid = spec.price != null && spec.price > 0
      if (!valid) {
        if (ref.current) {
          series.removePriceLine(ref.current)
          ref.current = null
        }
        return
      }
      const opts = {
        price: spec.price!,
        color: spec.color,
        lineWidth: spec.lineWidth,
        lineStyle: spec.style,
        axisLabelVisible: true,
        title: spec.title,
      }
      if (ref.current) {
        ref.current.applyOptions(opts)
      } else {
        ref.current = series.createPriceLine(opts)
      }
    }

    sync(marketBidLineRef, {
      price: strat?.marketBid,
      color: CHART_THEME.marketBid,
      style: LineStyle.Solid,
      lineWidth: 1,
      title: 'mkt bid',
    })
    sync(marketAskLineRef, {
      price: strat?.marketAsk,
      color: CHART_THEME.marketAsk,
      style: LineStyle.Solid,
      lineWidth: 1,
      title: 'mkt ask',
    })
    sync(ourBidLineRef, {
      price: strat?.bidOrderLive ? strat.bidPrice : undefined,
      color: strat?.bidSuppressed ? CHART_THEME.suppressed : CHART_THEME.ourBid,
      style: strat?.bidSuppressed ? LineStyle.Dashed : LineStyle.Solid,
      lineWidth: 2,
      title: strat?.bidSuppressed ? `bid (${strat.bidSuppressReason})` : 'bid',
    })
    sync(ourAskLineRef, {
      price: strat?.askOrderLive ? strat.askPrice : undefined,
      color: strat?.askSuppressed ? CHART_THEME.suppressed : CHART_THEME.ourAsk,
      style: strat?.askSuppressed ? LineStyle.Dashed : LineStyle.Solid,
      lineWidth: 2,
      title: strat?.askSuppressed ? `ask (${strat.askSuppressReason})` : 'ask',
    })
    // Reservation only shown when it's off the market mid by > 0.1 bps
    // — on a fresh flat book reservation == mid and a third line on top
    // of the mid is visual noise.
    const showReservation =
      strat?.reservation &&
      strat?.mid &&
      Math.abs((strat.reservation - strat.mid) / strat.mid) > 1e-5
    sync(reservationLineRef, {
      price: showReservation ? strat.reservation : undefined,
      color: CHART_THEME.reservation,
      style: LineStyle.Dashed,
      lineWidth: 1,
      title: 'reserv',
    })
  }, [strat])

  return <div className="chart-host" ref={hostRef} />
}
