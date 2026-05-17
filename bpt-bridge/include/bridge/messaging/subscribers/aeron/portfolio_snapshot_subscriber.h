#pragma once

/// @file
/// Aeron-backed strategy portfolio-snapshot subscriber. Owns the
/// FragmentAssembler so multi-fragment messages (OptionsMaker's
/// strategyState JSON exceeds the ~1376-byte single-fragment MTU once
/// active_strikes is populated for more than ~6 strikes) reassemble
/// before the handler fires.

#include "bridge/messaging/subscribers/api/portfolio_snapshot_subscriber.h"

#include <Aeron.h>
#include <FragmentAssembler.h>

#include <cstdint>
#include <memory>
#include <string>

namespace bpt::bridge::messaging::aeron {

class PortfolioSnapshotSubscriber final : public api::PortfolioSnapshotSubscriber {
public:
    PortfolioSnapshotSubscriber(std::shared_ptr<::aeron::Aeron> aeron,
                                const std::string& channel,
                                int32_t stream_id);

    int poll(int fragment_limit = 1) override;

private:
    std::shared_ptr<::aeron::Subscription> sub_;
    std::unique_ptr<::aeron::FragmentAssembler> assembler_;
};

}  // namespace bpt::bridge::messaging::aeron
