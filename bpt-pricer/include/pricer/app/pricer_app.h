#pragma once

#include "pricer/config/settings.h"
#include "pricer/messaging/aeron_bus.h"
#include "pricer/surface/surface_builder.h"

#include <messages/ExchangeId.h>

#include <bpt_app/app.h>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace bpt::pricer {

class PricerApp : public bpt::app::IService {
public:
    PricerApp(config::Settings settings, messaging::PricerBus bus);
    void run() override;

private:
    struct PerpInfo {
        std::string underlying;
        bpt::messages::ExchangeId::Value exchange_id;
    };

    config::Settings settings_;
    surface::SurfaceBuilder builder_;
    messaging::PricerBus bus_;
    std::unordered_map<uint64_t, PerpInfo> perp_map_;
};

}  // namespace bpt::pricer
