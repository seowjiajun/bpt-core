#pragma once

#include "book/adapter/i_balance_adapter.h"
#include "book/config/settings.h"
#include "book/messaging/aeron_bus.h"

#include <memory>
#include <vector>
#include <bpt_app/app.h>

namespace bpt::book {

class BookApp final : public bpt::app::IService {
public:
    BookApp(config::Settings settings, messaging::BookBus bus);

    void run() override;

private:
    config::Settings settings_;
    messaging::BookBus bus_;
    std::vector<std::unique_ptr<adapter::IBalanceAdapter>> adapters_;

    uint64_t correlation_id_{0};
};

}  // namespace bpt::book
