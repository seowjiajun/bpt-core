import { useEffect, useState } from 'react'
import { useStore } from '../store'
import type { ConnectionStatus, RunMode } from '../types/messages'
import { KillSwitch } from './KillSwitch'

const STATUS_LABEL: Record<ConnectionStatus, string> = {
  live: 'LIVE',
  mock: 'MOCK',
  halted: 'HALTED',
  off: 'DISCONNECTED',
}

const MODE_LABEL: Record<RunMode, string> = {
  paper: 'PAPER',
  live: 'LIVE',
  mock: 'MOCK',
}

function Clock() {
  const [time, setTime] = useState(() => new Date())
  useEffect(() => {
    const id = setInterval(() => setTime(new Date()), 1000)
    return () => clearInterval(id)
  }, [])
  return <span className="topbar-clock">{time.toUTCString().slice(17, 25)} UTC</span>
}

export function TopBar() {
  const mode = useStore((s) => s.mode)
  const symbol = useStore((s) => s.symbol)
  const strategy = useStore((s) => s.strategy)
  const exchange = useStore((s) => s.exchange)
  const instrumentType = useStore((s) => s.instrumentType)
  const price = useStore((s) => s.price)
  const firstPrice = useStore((s) => s.firstPrice)
  const status = useStore((s) => s.status)

  const change = price - firstPrice
  const pct = firstPrice ? (change / firstPrice) * 100 : 0
  const up = change >= 0

  // Render "<Strategy> · <SYMBOL> @ <EXCHANGE>" with graceful fallback
  // when fields aren't set yet (mock replay without a session message).
  const subjectParts: string[] = []
  if (strategy) subjectParts.push(strategy)
  if (symbol) {
    subjectParts.push(exchange ? `${symbol} @ ${exchange}` : symbol)
  }
  const subject = subjectParts.join(' · ') || '—'

  return (
    <div className="topbar">
      <span className="topbar-logo">BPT</span>
      <div className="topbar-divider" />
      <span className={`mode-pill mode-pill--${mode}`}>{MODE_LABEL[mode]}</span>
      <div className="topbar-divider" />
      <span className="topbar-symbol">{subject}</span>
      {instrumentType && (
        <span className={`inst-type-pill inst-type-pill--${instrumentType.toLowerCase()}`}>
          {instrumentType}
        </span>
      )}
      <span className="topbar-price">
        {price
          ? price.toLocaleString('en-US', { minimumFractionDigits: 1, maximumFractionDigits: 1 })
          : '—'}
      </span>
      {firstPrice > 0 && (
        <span className={`topbar-change ${up ? 'topbar-change--up' : 'topbar-change--down'}`}>
          {up ? '+' : ''}
          {change.toFixed(1)} ({up ? '+' : ''}
          {pct.toFixed(2)}%)
        </span>
      )}

      <div className="topbar-spacer" />

      <KillSwitch />
      <div className="topbar-divider" />

      <div className="topbar-status">
        <div className={`status-dot status-dot--${status}`} />
        {STATUS_LABEL[status]}
      </div>
      <div className="topbar-divider" />
      <Clock />
    </div>
  )
}
