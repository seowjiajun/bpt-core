import type { StrategyStateMsg } from '../../types/messages'

// Fallback panel for strategy kinds that don't have a dedicated component
// yet. Dumps the JSON so operators can still see what the strategy is
// emitting while the bespoke panel is being built.
export function GenericStrategyPanel({ state }: { state: StrategyStateMsg | null }) {
  return (
    <div className="panel" style={{ gridArea: 'stratstate' }}>
      <div className="panel-header">
        <span className="panel-title">Strategy State</span>
        <span className="panel-badge">{state?.kind ?? '—'}</span>
      </div>
      {state ? (
        <pre style={{ padding: '8px 12px', margin: 0, fontSize: 11, color: 'var(--text-muted)', overflow: 'auto' }}>
          {JSON.stringify(state, null, 2)}
        </pre>
      ) : (
        <div style={{ padding: '12px 16px', color: 'var(--text-muted)', fontSize: 13 }}>
          Waiting for strategy data.
        </div>
      )}
    </div>
  )
}
