#pragma once

/// \file
/// \brief Aeron-backed implementation of IMdControlSource.
///
/// Wraps a `bpt::common::aeron::Subscriber` over the control channel
/// and decodes each delivered fragment as an SBE `MdSubscribeBatch`
/// before invoking the user-supplied handler. Constructed by
/// AeronBus::build() at the prod composition root.
///
/// Threading: the underlying Aeron Subscriber is single-poll-thread;
/// poll() is invoked from MdGatewayService's main thread only.

#include "md_gateway/messaging/i_md_control_source.h"

#include <Aeron.h>

#include <bpt_common/aeron/subscriber.h>
#include <memory>
#include <string>

namespace bpt::md_gateway::messaging {

/// \brief Aeron-backed concrete for IMdControlSource.
///
/// Marked `final` — there is one prod implementation; no further
/// derivation is intended.
class MdControlSubscriber final : public IMdControlSource {
public:
    /// \brief Open an Aeron Subscriber on the given control channel + stream.
    MdControlSubscriber(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int stream_id);

    /// \copydoc IMdControlSource::poll
    int poll(BatchHandler handler) override;

private:
    std::unique_ptr<bpt::common::aeron::Subscriber> subscription_;

    /// Stashed across the Subscriber's per-fragment callback so it can
    /// be invoked in turn — keeps the lambda capture out of the hot path.
    BatchHandler current_handler_;
};

}  // namespace bpt::md_gateway::messaging
