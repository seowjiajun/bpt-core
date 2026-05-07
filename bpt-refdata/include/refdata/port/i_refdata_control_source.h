#pragma once

/// \file
/// \brief Inbound port: control-plane source delivering decoded RefdataRequests.

#include "refdata/messaging/messages.h"

#include <functional>

namespace bpt::refdata::port {

class IRefdataControlSource {
public:
    using RequestHandler = std::function<void(const messaging::RefdataRequest&)>;

    virtual ~IRefdataControlSource() = default;

    /// \brief Drain available control fragments, dispatching each to `handler`.
    /// \return Number of fragments processed; 0 means idle (caller drives idle strategy).
    virtual int poll(RequestHandler handler) = 0;
};

}  // namespace bpt::refdata::port
