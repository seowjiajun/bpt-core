#pragma once

// Position reconciliation: compare the strategy-side PositionTracker
// against the exchange's reported positions (AccountSnapshot) and
// report divergences. A divergence = "we think we hold X, exchange says
// we hold Y, and |X-Y| exceeds the threshold."
//
// Silent drift between the two views is a classic failure mode: a
// dropped fill event, a mis-scaled qty, a reject we handled wrong, or
// the OFI-style "divided by 1e8 twice" scale bug. Reconciliation
// surfaces it quickly as a loud log line instead of eventually as an
// unexplained P&L delta.
//
// This module is pure logic — no I/O, no refdata coupling. Callers
// (strategy_app) supply the instrument_id → exchange_symbol mapping
// already resolved from refdata. That keeps the diff trivially
// unit-testable with synthetic maps + AccountSnapshot.

#include <messages/AccountSnapshot.h>
#include <messages/ExchangeId.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bpt::strategy::strategy {

class PositionTracker;

struct Divergence {
    uint64_t instrument_id;
    bpt::messages::ExchangeId::Value exchange_id;
    std::string exchange_symbol;  // "" if the exchange didn't report a position for this instrument
    int64_t our_net_qty_e8;
    int64_t exchange_net_qty_e8;
    int64_t diff_e8;  // our - exchange (signed)
};

// Reconcile our PositionTracker against the positions carried in snap.
//
// Inputs:
//   tracker                    — our view, single source of truth for strategy-side position
//   snap                       — exchange's view, just arrived on the AccountSnapshot stream
//   instrument_id_to_symbol    — mapping we consult to match our tracker entries against
//                                 snap.positions[] (which is keyed by exchangeSymbol).
//                                 Typically built by the strategy from its state_ table.
//   threshold_e8               — divergences below this absolute value are ignored (rounding
//                                 noise, exchange reporting lag). Typical value: 1 qty-unit
//                                 (1e4 = 0.0001 in 1e8 scale, ~$10 at BTC prices).
//
// Returns a list of divergences. Empty vector = clean reconciliation.
// Positions reported by the exchange for instruments NOT in our
// instrument_id_to_symbol map are SILENTLY IGNORED — those aren't
// ours to track (e.g. pre-existing ETH balance in an OKX account
// that's not part of this strategy's universe).
std::vector<Divergence> reconcile(const PositionTracker& tracker,
                                  bpt::messages::AccountSnapshot& snap,
                                  const std::unordered_map<uint64_t, std::string>& instrument_id_to_symbol,
                                  int64_t threshold_e8);

// Drain the AccountSnapshot positions group into an
// exchange_symbol → net_qty_e8 map. Exposed so callers that want to
// both reconcile AND cache snapshot positions (e.g. for shutdown
// flatten's exchange-authoritative path) can iterate the SBE cursor
// exactly once rather than twice. Also useful in unit tests that want
// to inspect the snapshot contents without going through reconcile.
std::unordered_map<std::string, int64_t> extract_exchange_positions(bpt::messages::AccountSnapshot& snap);

// Drain the AccountSnapshot currencyBalances group into a
// ccy → equity_e8 map. For SPOT reconciliation: exchange holdings
// land in the base-currency equity row rather than in positions[],
// so callers compute delta = current_equity - initial_equity and
// compare that against PositionTracker.net_qty to detect drift.
// Order of call vs extract_exchange_positions() matters only if the
// same snapshot is passed to both — SBE cursors advance, so always
// positions first, then currency balances.
std::unordered_map<std::string, int64_t> extract_exchange_currency_balances(bpt::messages::AccountSnapshot& snap);

// reconcile() overload operating on an already-extracted map. Used
// together with extract_exchange_positions() when a caller needs both
// the divergence list and the raw snapshot positions.
std::vector<Divergence> reconcile(const PositionTracker& tracker,
                                  const std::unordered_map<std::string, int64_t>& exchange_by_symbol,
                                  bpt::messages::ExchangeId::Value exchange_id,
                                  const std::unordered_map<uint64_t, std::string>& instrument_id_to_symbol,
                                  int64_t threshold_e8);

}  // namespace bpt::strategy::strategy
