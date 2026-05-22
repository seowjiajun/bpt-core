#pragma once

#include "strategy/md/i_md_client.h"

#include <cstdint>
#include <vector>

namespace bpt::strategy::md {

// StateMap = map<uint64_t, State> where State has `.exchange` + `.symbol`.
template <typename StateMap>
[[nodiscard]] std::vector<IMdClient::InstrumentDesc> build_subscriptions(const StateMap& state,
                                                                         uint8_t depth = 0) {
    std::vector<IMdClient::InstrumentDesc> subs;
    subs.reserve(state.size());
    for (const auto& [id, st] : state)
        subs.push_back({id, st.exchange, st.symbol, depth});
    return subs;
}

}  // namespace bpt::strategy::md
