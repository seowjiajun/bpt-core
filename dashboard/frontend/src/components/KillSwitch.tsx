import { useEffect, useState } from 'react'
import { useStore } from '../store'

// Kill switch control — renders a red HALT button (or yellow RESUME when
// already halted) and a confirmation modal.
//
// Slice (a): clicks drive the store's localHalt/localResume actions with
// no server round-trip. Slice (b) will replace those with WS command
// sends that wait for a server-side status ack.
//
// UX principles:
//   - Kill switch is always visible — never buried under a menu
//   - Click is never one-step — there is always a confirm
//   - Halted state is loud and unmissable (button colour + persistent banner)
//   - Escape key and click-outside-modal both cancel
export function KillSwitch() {
  const status = useStore((s) => s.status)
  const mode = useStore((s) => s.mode)
  const halt = useStore((s) => s.halt)
  const resume = useStore((s) => s.resume)

  const isHalted = status === 'halted'
  const [confirming, setConfirming] = useState<'halt' | 'resume' | null>(null)

  // Close the modal on Escape so a panicked trader can back out cleanly.
  useEffect(() => {
    if (!confirming) return
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') setConfirming(null)
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [confirming])

  const handleConfirm = () => {
    if (confirming === 'halt') halt()
    else if (confirming === 'resume') resume()
    setConfirming(null)
  }

  return (
    <>
      <button
        type="button"
        className={`kill-switch ${isHalted ? 'kill-switch--resume' : 'kill-switch--halt'}`}
        onClick={() => setConfirming(isHalted ? 'resume' : 'halt')}
        title={isHalted ? 'Resume trading' : 'Halt trading — block new orders'}
      >
        {isHalted ? '▶ RESUME' : '■ HALT'}
      </button>

      {confirming && (
        <div className="modal-backdrop" onClick={() => setConfirming(null)}>
          <div className="modal" onClick={(e) => e.stopPropagation()}>
            <div className="modal-title">
              {confirming === 'halt' ? 'Halt trading?' : 'Resume trading?'}
            </div>
            <div className="modal-body">
              {confirming === 'halt' ? (
                <>
                  <p>The strategy will stop emitting new orders immediately.</p>
                  <p className="modal-note">
                    Open orders remain active. Existing positions are not flattened.
                  </p>
                  <p className="modal-note">
                    Mode: <span className={`mode-pill mode-pill--${mode}`}>{mode.toUpperCase()}</span>
                  </p>
                </>
              ) : (
                <p>Trading will resume. The strategy will start placing new orders.</p>
              )}
            </div>
            <div className="modal-actions">
              <button type="button" className="btn btn--ghost" onClick={() => setConfirming(null)}>
                Cancel
              </button>
              <button
                type="button"
                className={confirming === 'halt' ? 'btn btn--danger' : 'btn btn--warn'}
                onClick={handleConfirm}
                autoFocus
              >
                {confirming === 'halt' ? 'HALT TRADING' : 'RESUME TRADING'}
              </button>
            </div>
          </div>
        </div>
      )}
    </>
  )
}
