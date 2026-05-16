#pragma once

/// @file
/// Subscribes to strategy's portfolio-snapshot stream and delivers the
/// JSON payload as a reassembled string. Owns the FragmentAssembler so
/// multi-fragment messages (OptionsMaker's strategyState JSON exceeds the
/// ~1376-byte single-fragment MTU once active_strikes is populated for
/// more than ~6 strikes) reassemble before the handler fires.

#include <Aeron.h>
#include <FragmentAssembler.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace bpt::bridge::messaging {

class PortfolioSnapshotSubscriber {
public:
    using JsonHandler = std::function<void(std::string_view json)>;

    PortfolioSnapshotSubscriber(std::shared_ptr<::aeron::Aeron> aeron,
                                const std::string& channel,
                                int32_t stream_id);

    void set_handler(JsonHandler h) { handler_ = std::move(h); }

    int poll(int fragment_limit = 1);

private:
    std::shared_ptr<::aeron::Subscription> sub_;
    JsonHandler handler_;
    std::unique_ptr<::aeron::FragmentAssembler> assembler_;
};

}  // namespace bpt::bridge::messaging
