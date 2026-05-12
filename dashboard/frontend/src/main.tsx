import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import './index.css'
import App from './App.tsx'
import ArchiveApp from './ArchiveApp.tsx'

// Hard split between the live trading console and the research archive.
// Traders should never be one click away from leaving the live view; the
// archive lives at its own URL and is opened deliberately.
const isArchive = window.location.pathname.startsWith('/archive')

createRoot(document.getElementById('root')!).render(
  <StrictMode>{isArchive ? <ArchiveApp /> : <App />}</StrictMode>
)
