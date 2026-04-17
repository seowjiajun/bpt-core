#pragma once

#include "pricer/config/settings.h"
#include "pricer/md/md_subscriber.h"
#include "pricer/messaging/status_publisher.h"
#include "pricer/messaging/vol_surface_publisher.h"
#include "pricer/refdata/refdata_subscriber.h"
#include "pricer/surface/surface_builder.h"

#include <messages/ExchangeId.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace bpt::pricer {

class PricerApp {
public:
    PricerApp(config::Settings settings, std::shared_ptr<aeron::Aeron> aeron);
    void run();

private:
    struct PerpInfo {
        std::string underlying;
        bpt::messages::ExchangeId::Value exchange_id;
    };

    config::Settings settings_;
    surface::SurfaceBuilder builder_;
    std::unique_ptr<messaging::VolSurfacePublisher> vol_pub_;
    std::unique_ptr<messaging::StatusPublisher> status_pub_;
    std::unique_ptr<md::MdSubscriber> md_sub_;
    std::unique_ptr<refdata::RefdataSubscriber> refdata_sub_;
    std::unordered_map<uint64_t, PerpInfo> perp_map_;
};

}  // namespace bpt::pricer
