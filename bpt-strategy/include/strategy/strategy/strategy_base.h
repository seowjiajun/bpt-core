#pragma once

#include "strategy/strategy/i_strategy.h"
#include "strategy/strategy/position_tracker.h"

#include <cstdint>
#include <unordered_map>

namespace bpt::strategy::strategy {

template <class InstrumentState>
class StrategyBase : public IStrategy {
protected:
    [[nodiscard]] InstrumentState* find_state(uint64_t instrument_id) {
        const auto it = state_.find(instrument_id);
        return it != state_.end() ? &it->second : nullptr;
    }

    std::unordered_map<uint64_t, InstrumentState> state_;
    PositionTracker positions_;
};

}  // namespace bpt::strategy::strategy
