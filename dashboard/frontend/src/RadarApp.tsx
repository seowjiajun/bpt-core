// /radar — Market Color view. Dedicated layout focused on bpt-radar's
// MarketColor output: options IV term structure / RR / skew / GEX / max pain
// today, room to grow for perp basis, flow color, vol regime, calendar.
//
// Deliberately NOT sharing layout or chrome with the live trading view
// (/) — radar is an analytics surface; live trading is an execution
// surface. Different audiences, different update cadences, different
// signals. Same WS client + store under the hood; the routing happens in
// main.tsx.

import { useEffect, useState } from 'react'
import './App.css'
import { connectWebSocket } from './ws/client'
import { useStore } from './store'
import { CalendarPanel } from './components/CalendarPanel'
import { OptionsPulsePanel } from './components/OptionsPulsePanel'
import type { MarketColorMsg } from './types/messages'

const WS_URL = (import.meta.env.VITE_WS_URL as string | undefined) ?? 'ws://localhost:8080'

export default function RadarApp() {
  const [connected, setConnected] = useState(false)
  const marketColor = useStore((s) => s.marketColor)

  useEffect(() => {
    const stop = connectWebSocket({
      url: WS_URL,
      onOpen: () => setConnected(true),
      onClose: () => setConnected(false),
    })
    return stop
  }, [])

  // Multi-underlying tab strip: stable alphabetical order, first selected by
  // default. Lifted to component state so the user's pick survives store
  // updates.
  const underlyings = marketColor ? Object.keys(marketColor).sort() : []
  const [selected, setSelected] = useState<string | null>(null)
  useEffect(() => {
    if (selected == null && underlyings.length > 0) setSelected(underlyings[0])
    if (selected != null && !underlyings.includes(selected) && underlyings.length > 0) setSelected(underlyings[0])
  }, [underlyings, selected])

  const active: MarketColorMsg | undefined = selected != null ? marketColor?.[selected] : undefined
  const exchanges = active ? active.exchange : '—'
  const lastUpdateMs = active ? Math.round((Date.now() * 1e6 - active.ts) / 1e6) : null

  return (
    <div className="radar-shell">
      <header className="radar-header">
        <div className="radar-title">
          <span className="radar-brand">bpt-radar</span>
          <span className="radar-subtitle">market color</span>
        </div>
        <div className="radar-meta">
          <span className={connected ? 'stat-value--green' : 'stat-value--red'}>
            {connected ? '● live' : '○ disconnected'}
          </span>
          <span style={{ color: 'var(--text-muted)' }}>
            venue: {exchanges}
            {lastUpdateMs != null && Number.isFinite(lastUpdateMs) ? `  ·  ${lastUpdateMs}ms ago` : ''}
          </span>
        </div>
      </header>

      <nav className="radar-tabs">
        {underlyings.length === 0 && (
          <span style={{ color: 'var(--text-muted)', fontSize: 13 }}>
            Waiting for radar data…
          </span>
        )}
        {underlyings.map((u) => (
          <button
            key={u}
            className={u === selected ? 'radar-tab radar-tab--active' : 'radar-tab'}
            onClick={() => setSelected(u)}
          >
            {u}
          </button>
        ))}
      </nav>

      <main className="radar-main">
        <OptionsPulsePanel underlying={selected} />
        <CalendarPanel underlying={selected} />

        {/* Placeholder rows for future MarketColor domains. Each will become
            its own panel once the corresponding radar analysis module lands. */}
        <section className="radar-coming-soon">
          <h3>Coming soon</h3>
          <ul>
            <li>Perp basis (perp mark vs spot mid, in bps)</li>
            <li>Flow color (buy/sell imbalance from trades)</li>
            <li>Vol regime (realized vol + classifier)</li>
          </ul>
        </section>
      </main>
    </div>
  )
}

