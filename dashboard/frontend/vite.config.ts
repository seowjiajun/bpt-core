import { defineConfig, type Plugin } from 'vite'
import react from '@vitejs/plugin-react'
import { readdirSync, readFileSync, statSync, existsSync } from 'node:fs'
import { join, resolve } from 'node:path'

// Resolve bpt-core/jormungandr/results from the frontend directory.
const RESULTS_DIR = resolve(__dirname, '../../jormungandr/results')

/**
 * Dev-server plugin that exposes completed backtest runs as a tiny JSON API.
 *
 *   GET /api/backtest-runs            → [{ name, startingCapital, finalEquity, ... }]
 *   GET /api/backtest-runs/:name      → { summary, trades, pnlCurve }
 *
 * This is dev-mode only.  Production would serve the same endpoints from the
 * C++ bridge once we wire up Boost.Beast HTTP routing.
 */
function backtestArchivePlugin(): Plugin {
  return {
    name: 'backtest-archive',
    configureServer(server) {
      server.middlewares.use('/api/backtest-runs', (req, res, next) => {
        try {
          const url = req.url ?? '/'
          if (req.method !== 'GET') return next()

          if (!existsSync(RESULTS_DIR)) {
            res.statusCode = 200
            res.setHeader('Content-Type', 'application/json')
            res.end('[]')
            return
          }

          // List handler: GET /api/backtest-runs
          if (url === '/' || url === '') {
            const dirs = readdirSync(RESULTS_DIR, { withFileTypes: true })
              .filter((d) => d.isDirectory())
              .map((d) => d.name)
              .sort()
              .reverse()
            const runs = dirs
              .map((name) => {
                const summaryPath = join(RESULTS_DIR, name, 'summary.json')
                if (!existsSync(summaryPath)) return null
                try {
                  const summary = JSON.parse(readFileSync(summaryPath, 'utf-8'))
                  const stat = statSync(summaryPath)
                  return {
                    name,
                    mtime: stat.mtimeMs,
                    starting_capital: summary.starting_capital,
                    final_equity: summary.final_equity,
                    total_pnl: summary.total_pnl,
                    return_pct: summary.return_pct,
                    max_drawdown_pct: summary.max_drawdown_pct,
                    sharpe_per_fill: summary.sharpe_per_fill,
                    total_fills: summary.total_fills,
                    win_rate_pct: summary.win_rate_pct,
                  }
                } catch {
                  return null
                }
              })
              .filter((r) => r !== null)
            res.statusCode = 200
            res.setHeader('Content-Type', 'application/json')
            res.end(JSON.stringify(runs))
            return
          }

          // Detail handler: GET /api/backtest-runs/:name
          const name = decodeURIComponent(url.slice(1).split('/')[0])
          const runDir = join(RESULTS_DIR, name)
          if (!existsSync(runDir) || !statSync(runDir).isDirectory()) {
            res.statusCode = 404
            res.end('{"error":"run not found"}')
            return
          }

          const summaryPath = join(runDir, 'summary.json')
          const tradesPath = join(runDir, 'trades.csv')
          const pnlPath = join(runDir, 'pnl_curve.csv')

          const summary = existsSync(summaryPath)
            ? JSON.parse(readFileSync(summaryPath, 'utf-8'))
            : null
          const trades = existsSync(tradesPath)
            ? parseTradesCsv(readFileSync(tradesPath, 'utf-8'))
            : []
          const pnlCurve = existsSync(pnlPath)
            ? parsePnlCsv(readFileSync(pnlPath, 'utf-8'))
            : []

          res.statusCode = 200
          res.setHeader('Content-Type', 'application/json')
          res.end(JSON.stringify({ name, summary, trades, pnlCurve }))
        } catch (e) {
          res.statusCode = 500
          res.end(JSON.stringify({ error: String(e) }))
        }
      })
    },
  }
}

// trades.csv schema:
// simulation_ts,exchange,symbol,order_id,client_order_id,side,type,qty,price,realized_pnl,equity
function parseTradesCsv(raw: string) {
  const lines = raw.trim().split('\n')
  if (lines.length < 2) return []
  const rows: Array<Record<string, string | number>> = []
  for (let i = 1; i < lines.length; i++) {
    const cols = lines[i].split(',')
    if (cols.length < 11) continue
    rows.push({
      ts: Number(cols[0]),
      exchange: cols[1],
      symbol: cols[2],
      orderId: Number(cols[3]),
      clientOrderId: cols[4],
      side: cols[5],
      type: cols[6],
      qty: Number(cols[7]),
      price: Number(cols[8]),
      realizedPnl: Number(cols[9]),
      equity: Number(cols[10]),
    })
  }
  return rows
}

// pnl_curve.csv schema:  ts_ns,equity
function parsePnlCsv(raw: string) {
  const lines = raw.trim().split('\n')
  if (lines.length < 2) return []
  const out: Array<{ ts: number; equity: number }> = []
  for (let i = 1; i < lines.length; i++) {
    const [ts, eq] = lines[i].split(',')
    out.push({ ts: Number(ts), equity: Number(eq) })
  }
  return out
}

export default defineConfig({
  plugins: [react(), backtestArchivePlugin()],
})
