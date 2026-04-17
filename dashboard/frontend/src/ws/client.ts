// Real WebSocket client — speaks the same message schema as the mock replay.
// On reconnect it uses exponential backoff (1s → 30s) so a bridge restart
// doesn't hammer the server.

import { useStore } from '../store'
import type { Msg } from '../types/messages'

interface ClientOptions {
  url: string
  onOpen?: () => void
  onClose?: () => void
}

const RECONNECT_MIN_MS = 1_000
const RECONNECT_MAX_MS = 30_000

const VALID_TYPES = new Set<string>([
  'session',
  'status',
  'tick',
  'fill',
  'position',
  'order',
  'portfolio',
  'account',
  'toxicity',
  'strategyState',
])

function isValidMessage(x: unknown): x is Msg {
  if (!x || typeof x !== 'object') return false
  const t = (x as { type?: unknown }).type
  return typeof t === 'string' && VALID_TYPES.has(t)
}

// Shared reference so the store (or any caller) can send commands to the
// bridge without holding a direct reference to the WebSocket instance.
let activeSocket: WebSocket | null = null

export function sendCommand(cmd: 'halt' | 'resume' | 'cancel_all') {
  if (!activeSocket || activeSocket.readyState !== WebSocket.OPEN) {
    console.warn('[ws] cannot send command — not connected')
    return
  }
  activeSocket.send(JSON.stringify({ type: 'command', cmd }))
}

export function connectWebSocket({ url, onOpen, onClose }: ClientOptions): () => void {
  let socket: WebSocket | null = null
  let stopped = false
  let backoffMs = RECONNECT_MIN_MS
  let reconnectTimer: number | null = null

  const dispatch = (msg: Msg) => useStore.getState().handleMessage(msg)

  const open = () => {
    if (stopped) return
    socket = new WebSocket(url)

    socket.onopen = () => {
      backoffMs = RECONNECT_MIN_MS
      activeSocket = socket
      // Don't reset the store on reconnect — exchange-authoritative state
      // (account balance/equity/positions) should persist until the next
      // snapshot naturally overwrites it, otherwise every reconnect causes
      // the equity curve to flip to 0 and holdings to disappear for ~5s.
      dispatch({ type: 'status', state: 'live' })
      onOpen?.()
    }

    socket.onmessage = (ev) => {
      try {
        const parsed: unknown = JSON.parse(ev.data as string)
        if (!isValidMessage(parsed)) {
          console.warn('[ws] unknown message type:', parsed)
          return
        }
        dispatch(parsed)
      } catch (e) {
        console.warn('[ws] parse error:', e)
      }
    }

    socket.onclose = () => {
      activeSocket = null
      onClose?.()
      dispatch({ type: 'status', state: 'off' })
      if (stopped) return
      reconnectTimer = window.setTimeout(open, backoffMs)
      backoffMs = Math.min(backoffMs * 2, RECONNECT_MAX_MS)
    }

    socket.onerror = () => {
      // onclose will fire immediately after, handle reconnect there
      socket?.close()
    }
  }

  open()

  return () => {
    stopped = true
    activeSocket = null
    if (reconnectTimer !== null) clearTimeout(reconnectTimer)
    socket?.close()
    socket = null
  }
}
