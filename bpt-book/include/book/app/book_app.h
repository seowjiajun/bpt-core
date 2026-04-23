#pragma once

#include "book/adapter/i_balance_adapter.h"
#include "book/config/settings.h"
#include "book/messaging/balance_snapshot_publisher.h"

#include <Aeron.h>

#include <memory>
#include <vector>
#include <bpt_app/app.h>

namespace bpt::book {

class BookApp final : public bpt::app::IService {
public:
    BookApp(config::Settings settings, std::shared_ptr<::aeron::Aeron> aeron);

    void run() override;

private:
    config::Settings settings_;
    std::shared_ptr<::aeron::Aeron> aeron_;
    std::unique_ptr<messaging::BalanceSnapshotPublisher> publisher_;
    std::vector<std::unique_ptr<adapter::IBalanceAdapter>> adapters_;

    uint64_t correlation_id_{0};
};

}  // namespace bpt::book
