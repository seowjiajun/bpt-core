#pragma once

/// \file
/// Port: passive MD subscriber. The per-frame dispatch path was lifted
/// to a CRTP-templated concrete subscriber — see
/// `aeron::MdSubscriber<Handler>` (Handler is `PricerService` in prod).
/// Pricer is a read-only consumer; it does not send subscription
/// requests through this port (see api::MdSubscribeClient for control).

namespace bpt::pricer::md::api {

class MdSubscriber {
public:
    virtual ~MdSubscriber() = default;

    /// Poll for fragments. Returns number of fragments processed.
    virtual int poll(int fragment_limit = 10) = 0;
};

}  // namespace bpt::pricer::md::api
