// Strategy-state panel registry.
//
// Each strategy that emits a get_strategy_state_json() payload registers
// the matching panel component here, keyed by the JSON's `kind` field.
// App.tsx looks up the panel for the currently active strategy state;
// unknown kinds (or null state) fall through to GenericStrategyPanel.
//
// To add a new strategy:
//   1. Add a new kind to StrategyKind in types/messages.ts and define
//      the per-strategy state interface.
//   2. Implement the C++ JSON emitter in IStrategy::get_strategy_state_json.
//   3. Build a panel component that takes {state: <YourState>}.
//   4. Register it below.

import type { FC } from 'react'
import type { StrategyKind, StrategyStateMsg } from '../../types/messages'
import { AvellanedaStoikovPanel } from './AvellanedaStoikovPanel'
import { FundingArbPanel } from './FundingArbPanel'
import { GenericStrategyPanel } from './GenericStrategyPanel'
import { OptionsMakerPanel } from './OptionsMakerPanel'

// Each panel accepts the narrowed strategy state for its kind. The
// registry erases that narrowing — the dispatcher in App.tsx narrows
// by kind before passing the state in.
type AnyPanel = FC<{ state: StrategyStateMsg }>

export const STRATEGY_PANELS: Record<StrategyKind, AnyPanel> = {
  AS: AvellanedaStoikovPanel as AnyPanel,
  FundingArb: FundingArbPanel as AnyPanel,
  OptionsMaker: OptionsMakerPanel as AnyPanel,
}

export { GenericStrategyPanel }
