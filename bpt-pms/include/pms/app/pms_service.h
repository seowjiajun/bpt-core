#pragma once

#include "pms/adapter/i_balance_adapter.h"
#include "pms/config/settings.h"
#include "pms/messaging/aeron_bus.h"

#include <bpt_app/app.h>
#include <memory>
#include <vector>

namespace bpt::pms {

class PmsService final : public bpt::app::IService {
public:
    PmsService(config::Settings settings, messaging::PmsBus bus);

    void run() override;

private:
    config::Settings settings_;
    messaging::PmsBus bus_;
    std::vector<std::unique_ptr<adapter::IBalanceAdapter>> adapters_;

    uint64_t correlation_id_{0};
};

}  // namespace bpt::pms
