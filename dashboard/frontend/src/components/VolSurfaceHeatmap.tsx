import { useEffect, useRef, useState } from 'react'
import type { VolSmileSlice, OptionLeg } from '../types/options'

// Vol surface as a strike × expiry heatmap. Color maps IV from cool
// (low vol) to hot (high vol). Position markers overlay where the
// portfolio has exposure.

interface Props {
  slices: VolSmileSlice[]
  legs?: OptionLeg[]
}

const MARGIN = { top: 10, right: 70, bottom: 30, left: 60 }

// Blue (low IV) → yellow → red (high IV)
function ivColor(iv: number, minIv: number, maxIv: number): string {
  const t = maxIv > minIv ? (iv - minIv) / (maxIv - minIv) : 0.5
  // 3-stop gradient: #1a2a3a → #d29922 → #f85149
  if (t < 0.5) {
    const s = t * 2
    const r = Math.round(26 + s * (210 - 26))
    const g = Math.round(42 + s * (153 - 42))
    const b = Math.round(58 + s * (34 - 58))
    return `rgb(${r},${g},${b})`
  } else {
    const s = (t - 0.5) * 2
    const r = Math.round(210 + s * (248 - 210))
    const g = Math.round(153 + s * (81 - 153))
    const b = Math.round(34 + s * (73 - 34))
    return `rgb(${r},${g},${b})`
  }
}

export function VolSurfaceHeatmap({ slices, legs = [] }: Props) {
  const containerRef = useRef<HTMLDivElement>(null)
  const canvasRef = useRef<HTMLCanvasElement>(null)
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
    const canvas = canvasRef.current
    if (!canvas || dims.w === 0 || slices.length === 0) return

    const dpr = window.devicePixelRatio || 1
    canvas.width = dims.w * dpr
    canvas.height = dims.h * dpr
    canvas.style.width = `${dims.w}px`
    canvas.style.height = `${dims.h}px`

    const ctx = canvas.getContext('2d')
    if (!ctx) return
    ctx.scale(dpr, dpr)
    ctx.clearRect(0, 0, dims.w, dims.h)

    const w = dims.w - MARGIN.left - MARGIN.right
    const h = dims.h - MARGIN.top - MARGIN.bottom

    // Collect unique strikes and expiries
    const strikes = [...new Set(slices.flatMap((s) => s.points.map((p) => p.strike)))].sort(
      (a, b) => a - b
    )
    const expiries = slices.map((s) => s.expiry).sort((a, b) => a - b)

    if (strikes.length === 0 || expiries.length === 0) return

    // Build IV lookup: calls only (puts have near-identical IV)
    const ivMap = new Map<string, number>()
    let minIv = Infinity,
      maxIv = -Infinity
    for (const s of slices) {
      for (const p of s.points) {
        if (p.optionSide !== 'CALL') continue
        const key = `${p.expiry}:${p.strike}`
        ivMap.set(key, p.iv)
        if (p.iv < minIv) minIv = p.iv
        if (p.iv > maxIv) maxIv = p.iv
      }
    }

    const cellW = w / strikes.length
    const cellH = h / expiries.length

    // Draw heatmap cells
    for (let ei = 0; ei < expiries.length; ei++) {
      for (let si = 0; si < strikes.length; si++) {
        const key = `${expiries[ei]}:${strikes[si]}`
        const iv = ivMap.get(key)
        if (iv === undefined) continue

        const x = MARGIN.left + si * cellW
        const y = MARGIN.top + ei * cellH

        ctx.fillStyle = ivColor(iv, minIv, maxIv)
        ctx.fillRect(x, y, cellW - 1, cellH - 1)

        // IV label inside cell if cells are big enough
        if (cellW > 40 && cellH > 18) {
          ctx.fillStyle = '#cdd9e5'
          ctx.font = '9px monospace'
          ctx.textAlign = 'center'
          ctx.textBaseline = 'middle'
          ctx.fillText(`${(iv * 100).toFixed(0)}`, x + cellW / 2, y + cellH / 2)
        }
      }
    }

    // X-axis labels (strikes)
    ctx.fillStyle = '#768390'
    ctx.font = '10px monospace'
    ctx.textAlign = 'center'
    ctx.textBaseline = 'top'
    const xLabelStride = Math.max(1, Math.floor(strikes.length / 8))
    for (let i = 0; i < strikes.length; i += xLabelStride) {
      const x = MARGIN.left + i * cellW + cellW / 2
      ctx.fillText(`${(strikes[i] / 1000).toFixed(0)}K`, x, MARGIN.top + h + 6)
    }

    // Y-axis labels (expiries)
    ctx.textAlign = 'right'
    ctx.textBaseline = 'middle'
    for (let i = 0; i < expiries.length; i++) {
      const label = slices.find((s) => s.expiry === expiries[i])?.label ?? String(expiries[i])
      const y = MARGIN.top + i * cellH + cellH / 2
      ctx.fillText(label, MARGIN.left - 8, y)
    }

    // Color scale legend
    const legendX = MARGIN.left + w + 12
    const legendH = h
    const legendW = 12
    for (let i = 0; i < legendH; i++) {
      const t = 1 - i / legendH
      const iv = minIv + t * (maxIv - minIv)
      ctx.fillStyle = ivColor(iv, minIv, maxIv)
      ctx.fillRect(legendX, MARGIN.top + i, legendW, 1)
    }
    ctx.fillStyle = '#768390'
    ctx.font = '9px monospace'
    ctx.textAlign = 'left'
    ctx.textBaseline = 'top'
    ctx.fillText(`${(maxIv * 100).toFixed(0)}%`, legendX + legendW + 4, MARGIN.top)
    ctx.textBaseline = 'bottom'
    ctx.fillText(`${(minIv * 100).toFixed(0)}%`, legendX + legendW + 4, MARGIN.top + legendH)

    // Position markers
    for (const leg of legs) {
      const si = strikes.indexOf(leg.strike)
      const ei = expiries.indexOf(leg.expiry)
      if (si === -1 || ei === -1) continue

      const cx = MARGIN.left + si * cellW + cellW / 2
      const cy = MARGIN.top + ei * cellH + cellH / 2
      const r = Math.min(8, Math.max(4, Math.abs(leg.qty) * 3))

      ctx.beginPath()
      ctx.arc(cx, cy, r, 0, Math.PI * 2)
      ctx.strokeStyle = leg.qty > 0 ? '#3fb950' : '#f85149'
      ctx.lineWidth = 2
      ctx.stroke()

      ctx.fillStyle = leg.qty > 0 ? '#3fb950' : '#f85149'
      ctx.font = 'bold 9px monospace'
      ctx.textAlign = 'center'
      ctx.textBaseline = 'bottom'
      ctx.fillText(`${leg.qty > 0 ? '+' : ''}${leg.qty}`, cx, cy - r - 2)
    }
  }, [slices, legs, dims])

  return (
    <div className="chart-host" ref={containerRef} style={{ position: 'relative' }}>
      <canvas ref={canvasRef} style={{ position: 'absolute', inset: 0 }} />
    </div>
  )
}
