import { useEffect, useState } from 'react'
import './App.css'
import { ArchiveList } from './components/ArchiveList'
import { ArchiveDetail } from './components/ArchiveDetail'

// Selected run is encoded in location.hash so the browser back button works
// and run URLs are shareable:
//   /archive              → list
//   /archive#<run-name>   → detail view of that run
function readRun(): string | null {
  const h = window.location.hash.replace(/^#/, '')
  return h.length > 0 ? decodeURIComponent(h) : null
}

export default function ArchiveApp() {
  const [run, setRun] = useState<string | null>(readRun)

  useEffect(() => {
    const onHash = () => setRun(readRun())
    window.addEventListener('hashchange', onHash)
    return () => window.removeEventListener('hashchange', onHash)
  }, [])

  const openRun = (name: string) => {
    window.location.hash = encodeURIComponent(name)
  }
  const backToList = () => {
    window.location.hash = ''
  }

  return (
    <div className="archive-shell">
      <div className="topbar topbar--archive">
        <span className="topbar-logo">BPT</span>
        <div className="topbar-divider" />
        <span className="mode-pill mode-pill--research">RESEARCH</span>
        <div className="topbar-divider" />
        <span className="topbar-symbol">
          {run ? (
            <>
              <a href="#" onClick={(e) => { e.preventDefault(); backToList() }} className="archive-back">
                ← runs
              </a>
              <span style={{ marginLeft: 12 }}>{run}</span>
            </>
          ) : (
            'Backtest archive'
          )}
        </span>
        <div className="topbar-spacer" />
        <span className="topbar-clock">OFFLINE · HISTORICAL</span>
      </div>

      {run ? <ArchiveDetail name={run} /> : <ArchiveList onOpen={openRun} />}
    </div>
  )
}
