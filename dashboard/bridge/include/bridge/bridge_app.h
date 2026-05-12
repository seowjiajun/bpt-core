#pragma once

// BridgeApp — Aeron → WebSocket forwarder for the dashboard. Wrapped
// as a bpt::app::IService so main.cpp follows the same lifecycle
// pattern as the rest of the bpt-core services.

#include "bridge/settings.h"

#include <Aeron.h>

#include <bpt_app/app.h>
#include <memory>

namespace bridge {

class BridgeApp : public bpt::app::IService {
public:
    BridgeApp(config::Settings settings, std::shared_ptr<aeron::Aeron> aeron)
        : settings_(std::move(settings)),
          aeron_(std::move(aeron)) {}

    void run() override;

private:
    config::Settings settings_;
    std::shared_ptr<aeron::Aeron> aeron_;
};

}  // namespace bridge
