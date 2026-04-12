import { useEffect, useRef, useState } from 'react'
import type { VolSmileSlice, OptionLeg } from '../types/options'

// Vol smile rendered as plain SVG — lightweight-charts is optimized for
// time-series, not strike-space. SVG gives us clean control over the
// X-axis (strike) and multi-series lines without fighting the library.

interface Props {
  slices: VolSmileSlice[]
  legs?: OptionLeg[]
  width?: number
  height?: number
}

const COLORS = ['#388bfd', '#d29922', '#3fb950', '#f85149']
const MARGIN = { top: 20, right: 60, bottom: 30, left: 50 }

export function VolSmileChart({ slices, legs = [] }: Props) {
  const containerRef = useRef<HTMLDivElement>(null)
  const svgRef = useRef<SVGSVGElement>(null)
  const [dims, setDims] = useState({ w: 0, h: 0 })

  useEffect(() => {
    if (!containerRef.current) return
    const ro = new ResizeObserver((entries) => {
      for (const e of entries) {
        setDims({ w: e.contentRect.width, h: e.contentRect.height })
      }
    })
    ro.observe(containerRef.current)
    return () => ro.disconnect()
  }, [])

  useEffect(() => {
    if (!svgRef.current || slices.length === 0 || dims.w === 0) return

    const cw = dims.w
    const ch = dims.h
    if (cw === 0 || ch === 0) return

    const w = cw - MARGIN.left - MARGIN.right
    const h = ch - MARGIN.top - MARGIN.bottom

    // Compute axis ranges across all slices
    let minStrike = Infinity, maxStrike = -Infinity
    let minIv = Infinity, maxIv = -Infinity
    for (const s of slices) {
      for (const p of s.points) {
        if (p.strike < minStrike) minStrike = p.strike
        if (p.strike > maxStrike) maxStrike = p.strike
        if (p.iv < minIv) minIv = p.iv
        if (p.iv > maxIv) maxIv = p.iv
      }
    }
    // Pad IV axis
    const ivPad = (maxIv - minIv) * 0.1
    minIv -= ivPad
    maxIv += ivPad

    const xScale = (strike: number) => ((strike - minStrike) / (maxStrike - minStrike)) * w
    const yScale = (iv: number) => h - ((iv - minIv) / (maxIv - minIv)) * h

    const svg = svgRef.current
    svg.setAttribute('viewBox', `0 0 ${cw} ${ch}`)

    // Build SVG content
    let content = ''

    // Grid lines
    const yTicks = 5
    for (let i = 0; i <= yTicks; i++) {
      const iv = minIv + (maxIv - minIv) * (i / yTicks)
      const y = MARGIN.top + yScale(iv)
      content += `<line x1="${MARGIN.left}" y1="${y}" x2="${MARGIN.left + w}" y2="${y}" stroke="#1c2333" stroke-width="1"/>`
      content += `<text x="${MARGIN.left + w + 6}" y="${y + 4}" fill="#768390" font-size="10" font-family="monospace">${(iv * 100).toFixed(0)}%</text>`
    }

    // X-axis labels (strikes)
    const xTicks = Math.min(slices[0]?.points.length ?? 0, 9)
    const uniqueStrikes = [...new Set(slices.flatMap((s) => s.points.map((p) => p.strike)))].sort((a, b) => a - b)
    const labelStrides = Math.max(1, Math.floor(uniqueStrikes.length / xTicks))
    for (let i = 0; i < uniqueStrikes.length; i += labelStrides) {
      const strike = uniqueStrikes[i]
      const x = MARGIN.left + xScale(strike)
      content += `<text x="${x}" y="${MARGIN.top + h + 18}" fill="#768390" font-size="10" font-family="monospace" text-anchor="middle">${(strike / 1000).toFixed(0)}K</text>`
    }

    // Smile lines — one per expiry, calls only (puts have near-identical IV)
    slices.forEach((slice, si) => {
      const calls = slice.points
        .filter((p) => p.optionSide === 'CALL')
        .sort((a, b) => a.strike - b.strike)
      if (calls.length < 2) return

      const d = calls
        .map((p, i) => `${i === 0 ? 'M' : 'L'} ${MARGIN.left + xScale(p.strike)} ${MARGIN.top + yScale(p.iv)}`)
        .join(' ')
      content += `<path d="${d}" fill="none" stroke="${COLORS[si % COLORS.length]}" stroke-width="1.5" opacity="0.9"/>`

      // Legend label at last point
      const last = calls[calls.length - 1]
      content += `<text x="${MARGIN.left + xScale(last.strike) + 4}" y="${MARGIN.top + yScale(last.iv) - 6}" fill="${COLORS[si % COLORS.length]}" font-size="10" font-family="monospace">${slice.label}</text>`
    })

    // Position markers — show where portfolio legs sit on the surface
    for (const leg of legs) {
      const x = MARGIN.left + xScale(leg.strike)
      const y = MARGIN.top + yScale(leg.iv)
      const color = leg.qty > 0 ? '#3fb950' : '#f85149'
      const r = Math.min(6, Math.max(3, Math.abs(leg.qty) * 3))
      content += `<circle cx="${x}" cy="${y}" r="${r}" fill="${color}" opacity="0.85" stroke="#0d1117" stroke-width="1.5"/>`
      content += `<text x="${x}" y="${y - r - 4}" fill="${color}" font-size="9" font-family="monospace" text-anchor="middle">${leg.qty > 0 ? '+' : ''}${leg.qty}</text>`
    }

    svg.innerHTML = content
  }, [slices, legs, dims])

  return (
    <div className="chart-host" ref={containerRef} style={{ position: 'relative' }}>
      <svg
        ref={svgRef}
        style={{ position: 'absolute', inset: 0, width: '100%', height: '100%' }}
      />
    </div>
  )
}
