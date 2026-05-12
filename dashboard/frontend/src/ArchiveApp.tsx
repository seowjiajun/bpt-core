import { useEffect, useState } from 'react'
import './App.css'
import { ArchiveList } from './components/ArchiveList'
import { ArchiveDetail } from './components/ArchiveDetail'
import { ArchiveDiff } from './components/ArchiveDiff'
import { ArchiveSweep } from './components/ArchiveSweep'

// Selected run is encoded in location.hash so the browser back button works
// and run URLs are shareable:
//   /archive                                → list
//   /archive#<run-name>                     → detail view of that run
//   /archive#diff:<runA>:<runB>             → diff view of the two runs
//   /archive#sweep:<runA>:<runB>:<runC>...  → 1D/2D sweep heatmap of N runs
type Route =
  | { kind: 'list' }
  | { kind: 'detail'; name: string }
  | { kind: 'diff'; a: string; b: string }
  | { kind: 'sweep'; runs: string[] }

function readRoute(): Route {
  const h = window.location.hash.replace(/^#/, '')
  if (h.length === 0) return { kind: 'list' }
  if (h.startsWith('diff:')) {
    const parts = h.slice(5).split(':')
    if (parts.length === 2) {
      return { kind: 'diff', a: decodeURIComponent(parts[0]), b: decodeURIComponent(parts[1]) }
    }
    return { kind: 'list' }
  }
  if (h.startsWith('sweep:')) {
    const parts = h
      .slice(6)
      .split(':')
      .filter((p) => p.length > 0)
    if (parts.length >= 2) {
      return { kind: 'sweep', runs: parts.map(decodeURIComponent) }
    }
    return { kind: 'list' }
  }
  return { kind: 'detail', name: decodeURIComponent(h) }
}

export default function ArchiveApp() {
  const [route, setRoute] = useState<Route>(readRoute)

  useEffect(() => {
    const onHash = () => setRoute(readRoute())
    window.addEventListener('hashchange', onHash)
    return () => window.removeEventListener('hashchange', onHash)
  }, [])

  const openRun = (name: string) => {
    window.location.hash = encodeURIComponent(name)
  }
  const openDiff = (a: string, b: string) => {
    window.location.hash = `diff:${encodeURIComponent(a)}:${encodeURIComponent(b)}`
  }
  const openSweep = (runs: string[]) => {
    window.location.hash = `sweep:${runs.map(encodeURIComponent).join(':')}`
  }
  const backToList = () => {
    window.location.hash = ''
  }

  let title: React.ReactNode
  if (route.kind === 'detail') {
    title = (
      <>
        <a
          href="#"
          onClick={(e) => {
            e.preventDefault()
            backToList()
          }}
          className="archive-back"
        >
          ← runs
        </a>
        <span style={{ marginLeft: 12 }}>{route.name}</span>
      </>
    )
  } else if (route.kind === 'diff') {
    title = (
      <>
        <a
          href="#"
          onClick={(e) => {
            e.preventDefault()
            backToList()
          }}
          className="archive-back"
        >
          ← runs
        </a>
        <span style={{ marginLeft: 12 }}>
          diff: {route.a} ↔ {route.b}
        </span>
      </>
    )
  } else if (route.kind === 'sweep') {
    title = (
      <>
        <a
          href="#"
          onClick={(e) => {
            e.preventDefault()
            backToList()
          }}
          className="archive-back"
        >
          ← runs
        </a>
        <span style={{ marginLeft: 12 }}>sweep: {route.runs.length} runs</span>
      </>
    )
  } else {
    title = 'Backtest archive'
  }

  return (
    <div className="archive-shell">
      <div className="topbar topbar--archive">
        <span className="topbar-logo">BPT</span>
        <div className="topbar-divider" />
        <span className="mode-pill mode-pill--research">RESEARCH</span>
        <div className="topbar-divider" />
        <span className="topbar-symbol">{title}</span>
        <div className="topbar-spacer" />
        <span className="topbar-clock">OFFLINE · HISTORICAL</span>
      </div>

      {route.kind === 'detail' && <ArchiveDetail name={route.name} />}
      {route.kind === 'diff' && <ArchiveDiff runA={route.a} runB={route.b} />}
      {route.kind === 'sweep' && <ArchiveSweep runs={route.runs} />}
      {route.kind === 'list' && (
        <ArchiveList onOpen={openRun} onCompare={openDiff} onSweep={openSweep} />
      )}
    </div>
  )
}
