import { useEffect } from 'react'
import './App.css'
import { TopBar } from './components/TopBar'
import { PositionPanel } from './components/PositionPanel'
import { RiskPanel } from './components/RiskPanel'
import { Blotter } from './components/Blotter'
import type { MockFill } from './mock/replay'
import { PriceChart } from './components/PriceChart'
import { EquityChart } from './components/EquityChart'
import { HaltedBanner } from './components/HaltedBanner'
import { OpenOrdersPanel } from './components/OpenOrdersPanel'
import { GreeksPanel } from './components/GreeksPanel'
import { OptionsPositionPanel } from './components/OptionsPositionPanel'
import { VolSmileChart } from './components/VolSmileChart'
import { VolSurfaceHeatmap } from './components/VolSurfaceHeatmap'
import { VolTermStructure } from './components/VolTermStructure'
import { RiskLimitsPanel, MOCK_LIMITS } from './components/RiskLimitsPanel'
import { startMockReplay } from './mock/replay'
import { connectWebSocket } from './ws/client'
import { useStore } from './store'
import { MOCK_LEGS, MOCK_GREEKS, MOCK_VOL_SURFACE, MOCK_SPOT } from './mock/options'

// VITE_WS_URL selects the data source:
//   unset or "mock"           → in-memory mock replay of trades.csv
//   ws://host:port             → connect to the real C++ bridge
const WS_URL = import.meta.env.VITE_WS_URL ?? 'mock'

// ── Static mock fills (the 22 round trips from trades.csv) ────────────────────
// Typed without `seq` — the store assigns it when each fill is dispatched.
const MOCK_FILLS: MockFill[] = [
  { ts: 1767721259502000000, orderId: 1,  side: 'BUY',  qty: 1, price: 92007.0, realizedPnl: 0,      equity: 100071.25 },
  { ts: 1767721274101000000, orderId: 2,  side: 'SELL', qty: 1, price: 92091.1, realizedPnl: 84.10,  equity: 100084.10 },
  { ts: 1767722407301000000, orderId: 3,  side: 'BUY',  qty: 1, price: 91300.1, realizedPnl: 0,      equity: 100084.05 },
  { ts: 1767722416101000000, orderId: 4,  side: 'SELL', qty: 1, price: 91328.9, realizedPnl: 28.80,  equity: 100112.90 },
  { ts: 1767734136301000000, orderId: 5,  side: 'SELL', qty: 1, price: 93168.4, realizedPnl: 0,      equity: 100101.55 },
  { ts: 1767734160101000000, orderId: 6,  side: 'BUY',  qty: 1, price: 93418.3, realizedPnl: -249.9, equity: 99863.00  },
  { ts: 1767798137604000000, orderId: 7,  side: 'BUY',  qty: 1, price: 91467.1, realizedPnl: 0,      equity: 99862.95  },
  { ts: 1767798145404000000, orderId: 8,  side: 'SELL', qty: 1, price: 91524.2, realizedPnl: 57.10,  equity: 99920.10  },
  { ts: 1767799039703000000, orderId: 9,  side: 'BUY',  qty: 1, price: 91150.3, realizedPnl: 0,      equity: 99927.05  },
  { ts: 1767799046603000000, orderId: 10, side: 'SELL', qty: 1, price: 91233.8, realizedPnl: 83.50,  equity: 100003.60 },
  { ts: 1767882390109000000, orderId: 11, side: 'BUY',  qty: 1, price: 89415.8, realizedPnl: 0,      equity: 99993.75  },
  { ts: 1767882396909000000, orderId: 12, side: 'SELL', qty: 1, price: 89470.4, realizedPnl: 54.60,  equity: 100058.20 },
  { ts: 1767945874402000000, orderId: 13, side: 'BUY',  qty: 1, price: 90439.1, realizedPnl: 0,      equity: 100068.15 },
  { ts: 1767945882302000000, orderId: 14, side: 'SELL', qty: 1, price: 90504.6, realizedPnl: 65.50,  equity: 100123.70 },
  { ts: 1767965403603000000, orderId: 15, side: 'SELL', qty: 1, price: 90831.9, realizedPnl: 0,      equity: 100118.90 },
  { ts: 1767965411503000000, orderId: 16, side: 'BUY',  qty: 1, price: 90652.0, realizedPnl: 179.9,  equity: 100303.60 },
  { ts: 1768161957005000000, orderId: 17, side: 'BUY',  qty: 1, price: 90445.0, realizedPnl: 0,      equity: 100313.55 },
  { ts: 1768161967505000000, orderId: 18, side: 'SELL', qty: 1, price: 90423.2, realizedPnl: -21.80, equity: 100281.80 },
  { ts: 1768172642005000000, orderId: 19, side: 'BUY',  qty: 1, price: 90220.1, realizedPnl: 0,      equity: 100285.00 },
  { ts: 1768172649905000000, orderId: 20, side: 'SELL', qty: 1, price: 90266.5, realizedPnl: 46.40,  equity: 100328.20 },
  { ts: 1768179609202000000, orderId: 21, side: 'SELL', qty: 1, price: 91419.9, realizedPnl: 0,      equity: 100352.55 },
  { ts: 1768179616002000000, orderId: 22, side: 'BUY',  qty: 1, price: 91331.8, realizedPnl: 88.10,  equity: 100416.30 },
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
        startingCapital: 100_000,
        intervalMs: 1200,
      })
    }
    return connectWebSocket({ url: WS_URL })
  }, [])

  const finalEquity = fills.length ? fills[fills.length - 1].equity : 100_000

  // Options data — use live store data if available, fall back to mock.
  const storeLegs = useStore((s) => s.optionLegs)
  const storeGreeks = useStore((s) => s.portfolioGreeks)
  const storeSurface = useStore((s) => s.volSurface)
  const price = useStore((s) => s.price)

  const hasLiveOptions = storeLegs.length > 0
  const optLegs = hasLiveOptions ? storeLegs : MOCK_LEGS
  const optGreeks = storeGreeks ?? MOCK_GREEKS
  const optSurface = storeSurface.length > 0 ? storeSurface : MOCK_VOL_SURFACE
  const spot = price || MOCK_SPOT

  // Show options panels in mock mode (dev) or when live options data arrives.
  const showOptions = WS_URL === 'mock' || hasLiveOptions

  return (
    <div className={`shell ${showOptions ? 'shell--options' : ''}`}>
      <TopBar />
      <HaltedBanner />

      <div className="main-row">
        <div className="panel">
          <div className="panel-header">
            <span className="panel-title">BTC-USDT · 1m</span>
            <span className="panel-badge">OKX</span>
          </div>
          <PriceChart />
        </div>

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
              <span className="panel-badge">{optSurface.length} expiries</span>
            </div>
            <VolSurfaceHeatmap slices={optSurface} legs={optLegs} />
          </div>
          <div className="panel">
            <div className="panel-header">
              <span className="panel-title">Vol Smile</span>
            </div>
            <VolSmileChart slices={optSurface} legs={optLegs} />
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

      <OpenOrdersPanel />
      <Blotter />
    </div>
  )
}
