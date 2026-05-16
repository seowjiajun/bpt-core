import { useEffect, useState } from 'react'
import './App.css'
import { TopBar } from './components/TopBar'
import { PositionPanel } from './components/PositionPanel'
import { RiskPanel } from './components/RiskPanel'
import { Blotter } from './components/Blotter'
import type { MockFill } from './mock/replay'
import { EquityChart } from './components/EquityChart'
import { HaltedBanner } from './components/HaltedBanner'
import { OpenOrdersPanel } from './components/OpenOrdersPanel'
import { HoldingsPanel } from './components/HoldingsPanel'
import { GreeksPanel } from './components/GreeksPanel'
import { OptionsPositionPanel } from './components/OptionsPositionPanel'
import { VolSmileChart } from './components/VolSmileChart'
import { VolSurfaceHeatmap } from './components/VolSurfaceHeatmap'
import { VolTermStructure } from './components/VolTermStructure'
import { RiskLimitsPanel, MOCK_LIMITS } from './components/RiskLimitsPanel'
import { ToxicityPanel } from './components/ToxicityPanel'
import { STRATEGY_PANELS, GenericStrategyPanel } from './components/panels'
import { STRATEGY_CHARTS, DefaultChart } from './components/charts'
import { startMockReplay } from './mock/replay'
import { connectWebSocket } from './ws/client'
import { useStore } from './store'
import { MOCK_LEGS, MOCK_GREEKS, MOCK_VOL_SURFACE, MOCK_SPOT } from './mock/options'
import type { PortfolioGreeks } from './types/options'

// VITE_WS_URL selects the data source:
//   unset or "mock"           → in-memory mock replay of trades.csv
//   ws://host:port             → connect to the real C++ bridge
const WS_URL = import.meta.env.VITE_WS_URL ?? 'mock'

// ── Static mock fills (the 22 round trips from trades.csv) ────────────────────
// Typed without `seq` — the store assigns it when each fill is dispatched.
const MOCK_FILLS: MockFill[] = [
  {
    ts: 1767721259502000000,
    orderId: 1,
    side: 'BUY',
    qty: 1,
    price: 92007.0,
    realizedPnl: 0,
    equity: 100071.25,
  },
  {
    ts: 1767721274101000000,
    orderId: 2,
    side: 'SELL',
    qty: 1,
    price: 92091.1,
    realizedPnl: 84.1,
    equity: 100084.1,
  },
  {
    ts: 1767722407301000000,
    orderId: 3,
    side: 'BUY',
    qty: 1,
    price: 91300.1,
    realizedPnl: 0,
    equity: 100084.05,
  },
  {
    ts: 1767722416101000000,
    orderId: 4,
    side: 'SELL',
    qty: 1,
    price: 91328.9,
    realizedPnl: 28.8,
    equity: 100112.9,
  },
  {
    ts: 1767734136301000000,
    orderId: 5,
    side: 'SELL',
    qty: 1,
    price: 93168.4,
    realizedPnl: 0,
    equity: 100101.55,
  },
  {
    ts: 1767734160101000000,
    orderId: 6,
    side: 'BUY',
    qty: 1,
    price: 93418.3,
    realizedPnl: -249.9,
    equity: 99863.0,
  },
  {
    ts: 1767798137604000000,
    orderId: 7,
    side: 'BUY',
    qty: 1,
    price: 91467.1,
    realizedPnl: 0,
    equity: 99862.95,
  },
  {
    ts: 1767798145404000000,
    orderId: 8,
    side: 'SELL',
    qty: 1,
    price: 91524.2,
    realizedPnl: 57.1,
    equity: 99920.1,
  },
  {
    ts: 1767799039703000000,
    orderId: 9,
    side: 'BUY',
    qty: 1,
    price: 91150.3,
    realizedPnl: 0,
    equity: 99927.05,
  },
  {
    ts: 1767799046603000000,
    orderId: 10,
    side: 'SELL',
    qty: 1,
    price: 91233.8,
    realizedPnl: 83.5,
    equity: 100003.6,
  },
  {
    ts: 1767882390109000000,
    orderId: 11,
    side: 'BUY',
    qty: 1,
    price: 89415.8,
    realizedPnl: 0,
    equity: 99993.75,
  },
  {
    ts: 1767882396909000000,
    orderId: 12,
    side: 'SELL',
    qty: 1,
    price: 89470.4,
    realizedPnl: 54.6,
    equity: 100058.2,
  },
  {
    ts: 1767945874402000000,
    orderId: 13,
    side: 'BUY',
    qty: 1,
    price: 90439.1,
    realizedPnl: 0,
    equity: 100068.15,
  },
  {
    ts: 1767945882302000000,
    orderId: 14,
    side: 'SELL',
    qty: 1,
    price: 90504.6,
    realizedPnl: 65.5,
    equity: 100123.7,
  },
  {
    ts: 1767965403603000000,
    orderId: 15,
    side: 'SELL',
    qty: 1,
    price: 90831.9,
    realizedPnl: 0,
    equity: 100118.9,
  },
  {
    ts: 1767965411503000000,
    orderId: 16,
    side: 'BUY',
    qty: 1,
    price: 90652.0,
    realizedPnl: 179.9,
    equity: 100303.6,
  },
  {
    ts: 1768161957005000000,
    orderId: 17,
    side: 'BUY',
    qty: 1,
    price: 90445.0,
    realizedPnl: 0,
    equity: 100313.55,
  },
  {
    ts: 1768161967505000000,
    orderId: 18,
    side: 'SELL',
    qty: 1,
    price: 90423.2,
    realizedPnl: -21.8,
    equity: 100281.8,
  },
  {
    ts: 1768172642005000000,
    orderId: 19,
    side: 'BUY',
    qty: 1,
    price: 90220.1,
    realizedPnl: 0,
    equity: 100285.0,
  },
  {
    ts: 1768172649905000000,
    orderId: 20,
    side: 'SELL',
    qty: 1,
    price: 90266.5,
    realizedPnl: 46.4,
    equity: 100328.2,
  },
  {
    ts: 1768179609202000000,
    orderId: 21,
    side: 'SELL',
    qty: 1,
    price: 91419.9,
    realizedPnl: 0,
    equity: 100352.55,
  },
  {
    ts: 1768179616002000000,
    orderId: 22,
    side: 'BUY',
    qty: 1,
    price: 91331.8,
    realizedPnl: 88.1,
    equity: 100416.3,
  },
]

export default function App() {
  const fills = useStore((s) => s.fills)

  useEffect(() => {
    if (WS_URL === 'mock') {
      return startMockReplay({
        fills: MOCK_FILLS,
        symbol: 'BTC-USDT',
        strategy: 'VwapReversion',
        exchange: 'OKX',
        intervalMs: 1200,
      })
    }
    return connectWebSocket({ url: WS_URL })
  }, [])

  const finalEquity = fills.length ? fills[fills.length - 1].equity : 0

  // Options data — strictly live store data. Mock fallback removed because
  // mocked smile/heatmap silently overlaid mock IV curves under real position
  // markers, which was confusing for multi-underlying ops. If you want a
  // dashboard demo without live services, use the bridge's `mock` URL mode.
  const storeLegs = useStore((s) => s.optionLegs)
  const storeGreeks = useStore((s) => s.portfolioGreeks)
  const storeSurface = useStore((s) => s.volSurface)
  const price = useStore((s) => s.price)
  const strategyState = useStore((s) => s.strategyState)

  const isMockMode = WS_URL === 'mock'
  const hasLiveOptions = storeLegs.length > 0
  const optLegs = isMockMode && !hasLiveOptions ? MOCK_LEGS : storeLegs
  // Zero-init greeks until the first portfolio frame lands — keeps GreeksPanel
  // non-null without falling back to fabricated mock values when running live.
  const ZERO_GREEKS: PortfolioGreeks = {
    netDelta: 0,
    netGamma: 0,
    netVega: 0,
    netTheta: 0,
    totalUnrealizedPnl: 0,
    totalRealizedPnl: 0,
  }
  const optGreeks = storeGreeks ?? (isMockMode ? MOCK_GREEKS : ZERO_GREEKS)
  const optSurface = isMockMode && storeSurface.length === 0 ? MOCK_VOL_SURFACE : storeSurface
  const spot = price || (isMockMode ? MOCK_SPOT : 0)

  // Underlying selector for vol surface + smile panels — derived from the
  // surface data so it auto-populates with whatever underlyings the strategy
  // emits. Default picks the underlying with the most surface points (so the
  // initial view always lands on a populated smile rather than a chain with
  // 2 stale strikes).
  const underlyingsSorted = Array.from(new Set(optSurface.map((s) => s.underlying))).sort()
  const pointCountByUnderlying = new Map<string, number>()
  for (const s of optSurface) {
    pointCountByUnderlying.set(s.underlying, (pointCountByUnderlying.get(s.underlying) ?? 0) + s.points.length)
  }
  const underlyings = underlyingsSorted
  const defaultUnderlying =
    [...pointCountByUnderlying.entries()].sort(([, a], [, b]) => b - a)[0]?.[0] ?? underlyings[0] ?? null
  const [selectedUnderlying, setSelectedUnderlying] = useState<string | null>(null)
  const activeUnderlying = selectedUnderlying ?? defaultUnderlying
  const filteredSurface = activeUnderlying
    ? optSurface.filter((s) => s.underlying === activeUnderlying)
    : optSurface
  const filteredLegs = activeUnderlying
    ? optLegs.filter((l) => l.underlying === activeUnderlying)
    : optLegs

  // Show options panels in mock mode (dev) or when live options data arrives.
  const showOptions = isMockMode || hasLiveOptions

  // Per-strategy state panel — picks the right component for the active
  // strategy's `kind`. Unknown kinds (or no state yet) fall back to the
  // generic JSON-dump panel.
  const strategyPanelNode = strategyState ? (
    (() => {
      const Panel = STRATEGY_PANELS[strategyState.kind] ?? GenericStrategyPanel
      return <Panel state={strategyState} />
    })()
  ) : (
    <GenericStrategyPanel state={null} />
  )

  // Per-strategy main chart. AS keeps the candlestick PriceChart;
  // FundingArb gets the dual-leg DualLegChart. Unknown kinds default
  // to PriceChart (works for any single-instrument feed).
  function MainChart() {
    const Chart = strategyState
      ? (STRATEGY_CHARTS[strategyState.kind] ?? DefaultChart)
      : DefaultChart
    return <Chart />
  }

  return (
    <div className={`shell ${showOptions ? 'shell--options' : ''} ${strategyState?.kind === 'OptionsMaker' ? 'shell--no-toxicity' : ''}`}>
      <TopBar />
      <HaltedBanner />

      <div className="main-row">
        <MainChart />

        <div className={`right-col ${showOptions ? 'right-col--options' : ''}`}>
          {showOptions ? <GreeksPanel greeks={optGreeks} /> : <PositionPanel />}
          {showOptions ? (
            <div className="panel">
              <div className="panel-header">
                <span className="panel-title">Term Structure</span>
                <span className="panel-badge">ATM IV</span>
              </div>
              <VolTermStructure slices={optSurface} spot={spot} />
            </div>
          ) : (
            <RiskPanel />
          )}
          {showOptions && <RiskLimitsPanel greeks={optGreeks} limits={MOCK_LIMITS} />}
        </div>
      </div>

      {showOptions && (
        <div className="options-row">
          <div className="panel">
            <div className="panel-header">
              <span className="panel-title">Vol Surface</span>
              <span className="panel-badge">
                {underlyings.length > 1 ? (
                  <span style={{ display: 'inline-flex', gap: 4 }}>
                    {underlyings.map((u) => (
                      <button
                        key={u}
                        onClick={() => setSelectedUnderlying(u)}
                        style={{
                          padding: '2px 8px',
                          fontSize: 10,
                          fontWeight: 600,
                          border: '1px solid var(--border)',
                          background: u === activeUnderlying ? 'var(--accent)' : 'transparent',
                          color: u === activeUnderlying ? 'var(--bg)' : 'var(--text-primary)',
                          borderRadius: 3,
                          cursor: 'pointer',
                        }}
                      >
                        {u}
                      </button>
                    ))}
                  </span>
                ) : (
                  `${activeUnderlying ?? '—'} · ${filteredSurface.length} expiries`
                )}
              </span>
            </div>
            <VolSurfaceHeatmap slices={filteredSurface} legs={filteredLegs} />
          </div>
          <div className="panel">
            <div className="panel-header">
              <span className="panel-title">Vol Smile</span>
              <span className="panel-badge">{activeUnderlying ?? '—'}</span>
            </div>
            <VolSmileChart slices={filteredSurface} legs={filteredLegs} />
          </div>
          <OptionsPositionPanel legs={optLegs} />
        </div>
      )}

      <div className="panel" style={{ gridArea: 'equity' }}>
        <div className="panel-header">
          <span className="panel-title">Equity Curve</span>
          <span className="panel-badge">
            ${finalEquity.toLocaleString('en-US', { minimumFractionDigits: 2 })}
          </span>
        </div>
        <EquityChart />
      </div>

      {strategyPanelNode}
      {strategyState?.kind !== 'OptionsMaker' && <ToxicityPanel />}
      <OpenOrdersPanel />
      <HoldingsPanel />
      <Blotter />
    </div>
  )
}
