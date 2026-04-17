#include "strategy/strategy/strategy_factory.h"

#include "strategy/md/md_client.h"
#include "strategy/order/order_gateway_client.h"
#include "strategy/order/order_manager.h"
#include "strategy/strategy/avellaneda_stoikov_strategy.h"
#include "strategy/strategy/funding_arb_strategy.h"
#include "strategy/strategy/hmm_strategy.h"
#include "strategy/strategy/momentum_strategy.h"
#include "strategy/strategy/ofi_strategy.h"
#include "strategy/strategy/regime_switch_strategy.h"
#include "strategy/strategy/short_vol_strategy.h"
#include "strategy/strategy/vwap_reversion_strategy.h"

#include <fmt/format.h>
#include <stdexcept>

namespace bpt::strategy::strategy {

std::unique_ptr<IStrategy> StrategyFactory::create(const config::EngineConfig& cfg,
                                                   refdata::RefdataClient& refdata,
                                                   md::MdClient* md,
                                                   order::OrderManager* order_mgr,
                                                   vol::VolSurfaceClient* vol_client) {
    const std::string& type = cfg.strategy.type;

    ygg::log::info("[StrategyFactory] Instantiating strategy type: {}", type);

    if (type == "MomentumStrategy") {
        return std::make_unique<MomentumStrategy>(cfg.correlation_id, cfg.strategy, refdata, md, order_mgr);
    }

    if (type == "OFIStrategy") {
        return std::make_unique<OFIStrategy>(cfg.correlation_id, cfg.strategy, refdata, md, order_mgr);
    }

    if (type == "VwapReversionStrategy") {
        return std::make_unique<VwapReversionStrategy>(cfg.correlation_id, cfg.strategy, refdata, md, order_mgr);
    }

    if (type == "AvellanedaStoikovStrategy") {
        return std::make_unique<AvellanedaStoikovStrategy>(cfg.correlation_id, cfg.strategy, refdata, md, order_mgr);
    }

    if (type == "FundingArbStrategy") {
        order::OrderGatewayClient* gw = order_mgr ? &order_mgr->gw() : nullptr;
        return std::make_unique<FundingArbStrategy>(cfg.correlation_id, cfg.strategy, refdata, md, gw);
    }

    if (type == "HmmStrategy") {
        return std::make_unique<HmmStrategy>(cfg.correlation_id, cfg.strategy, refdata, md, order_mgr);
    }

    if (type == "RegimeSwitchStrategy") {
        return std::make_unique<RegimeSwitchStrategy>(cfg.correlation_id, cfg.strategy, refdata, md, order_mgr);
    }

    if (type == "ShortVolStrategy") {
        return std::make_unique<ShortVolStrategy>(cfg.correlation_id, cfg.strategy, refdata, md, order_mgr, vol_client);
    }

    throw std::invalid_argument(fmt::format("Unknown strategy type: {}", type));
}

}  // namespace bpt::strategy::strategy
