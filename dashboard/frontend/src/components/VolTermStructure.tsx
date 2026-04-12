import { useEffect, useRef, useState } from 'react'
import type { VolSmileSlice } from '../types/options'

// ATM implied vol vs time-to-expiry. Plots the ATM call IV for each
// expiry slice — the strike closest to spot is treated as ATM.

interface Props {
  slices: VolSmileSlice[]
  spot: number
}

const MARGIN = { top: 16, right: 16, bottom: 28, left: 50 }

export function VolTermStructure({ slices, spot }: Props) {
  const containerRef = useRef<HTMLDivElement>(null)
  const svgRef = useRef<SVGSVGElement>(null)
  const [dims, setDims] = useState({ w: 0, h: 0 })

  useEffect(() => {
    if (!containerRef.current) return
    const ro = new ResizeObserver((entries) => {
      for (const e of entries) setDims({ w: e.contentRect.width, h: e.contentRect.height })
    })
    ro.observe(containerRef.current)
    return () => ro.disconnect()
  }, [])

  useEffect(() => {
    if (!svgRef.current || dims.w === 0 || slices.length === 0) return

    const w = dims.w - MARGIN.left - MARGIN.right
    const h = dims.h - MARGIN.top - MARGIN.bottom

    // Find ATM IV for each expiry (call closest to spot)
    const points: Array<{ dte: number; iv: number; label: string }> = []
    for (const s of slices) {
      const calls = s.points.filter((p) => p.optionSide === 'CALL')
      if (calls.length === 0) continue
      const atm = calls.reduce((best, p) =>
        Math.abs(p.strike - spot) < Math.abs(best.strike - spot) ? p : best
      )
      points.push({ dte: s.daysToExpiry, iv: atm.iv, label: s.label })
    }
    points.sort((a, b) => a.dte - b.dte)

    if (points.length === 0) return

    const minDte = 0
    const maxDte = Math.max(...points.map((p) => p.dte)) * 1.15
    const ivs = points.map((p) => p.iv)
    const ivPad = (Math.max(...ivs) - Math.min(...ivs)) * 0.25 || 0.02
    const minIv = Math.min(...ivs) - ivPad
    const maxIv = Math.max(...ivs) + ivPad

    const xScale = (dte: number) => ((dte - minDte) / (maxDte - minDte)) * w
    const yScale = (iv: number) => h - ((iv - minIv) / (maxIv - minIv)) * h

    const svg = svgRef.current
    svg.setAttribute('viewBox', `0 0 ${dims.w} ${dims.h}`)

    let content = ''

    // Grid lines
    const yTicks = 4
    for (let i = 0; i <= yTicks; i++) {
      const iv = minIv + (maxIv - minIv) * (i / yTicks)
      const y = MARGIN.top + yScale(iv)
      content += `<line x1="${MARGIN.left}" y1="${y}" x2="${MARGIN.left + w}" y2="${y}" stroke="#1c2333" stroke-width="1"/>`
      content += `<text x="${MARGIN.left - 6}" y="${y + 3}" fill="#768390" font-size="10" font-family="monospace" text-anchor="end">${(iv * 100).toFixed(1)}%</text>`
    }

    // Line connecting points
    if (points.length >= 2) {
      const d = points
        .map((p, i) => `${i === 0 ? 'M' : 'L'} ${MARGIN.left + xScale(p.dte)} ${MARGIN.top + yScale(p.iv)}`)
        .join(' ')
      content += `<path d="${d}" fill="none" stroke="#388bfd" stroke-width="2" opacity="0.9"/>`
    }

    // Data points + labels
    for (const p of points) {
      const cx = MARGIN.left + xScale(p.dte)
      const cy = MARGIN.top + yScale(p.iv)
      content += `<circle cx="${cx}" cy="${cy}" r="4" fill="#388bfd" stroke="#0d1117" stroke-width="1.5"/>`
      content += `<text x="${cx}" y="${MARGIN.top + h + 16}" fill="#768390" font-size="10" font-family="monospace" text-anchor="middle">${p.label}</text>`
      content += `<text x="${cx + 8}" y="${cy - 8}" fill="#cdd9e5" font-size="10" font-family="monospace">${(p.iv * 100).toFixed(1)}%</text>`
    }

    // X-axis label
    content += `<text x="${MARGIN.left + w / 2}" y="${dims.h - 2}" fill="#444c56" font-size="9" font-family="monospace" text-anchor="middle">EXPIRY</text>`

    svg.innerHTML = content
  }, [slices, spot, dims])

  return (
    <div className="chart-host" ref={containerRef} style={{ position: 'relative' }}>
      <svg
        ref={svgRef}
        style={{ position: 'absolute', inset: 0, width: '100%', height: '100%' }}
      />
    </div>
  )
}
