#pragma once

#include "strategy/config/config.h"
#include "strategy/md/md_client.h"
#include "strategy/order/order_manager.h"
#include "strategy/refdata/refdata_client.h"
#include "strategy/strategy/i_strategy.h"
#include "strategy/vol/vol_surface_client.h"

#include <memory>

namespace bpt::strategy::strategy {

class StrategyFactory {
public:
    // Creates and returns a strategy based on the AppConfig parameters.
    // md, order_mgr, and vol_client are optional (nullptr if not configured).
    // Throws std::invalid_argument if the strategy type is unknown.
    static std::unique_ptr<IStrategy> create(const config::EngineConfig& cfg,
                                             refdata::RefdataClient& refdata,
                                             md::MdClient* md,
                                             order::OrderManager* order_mgr,
                                             vol::VolSurfaceClient* vol_client = nullptr);
};

}  // namespace bpt::strategy::strategy
