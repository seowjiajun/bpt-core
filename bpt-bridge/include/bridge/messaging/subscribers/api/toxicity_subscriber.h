#pragma once

/// @file
/// Port: bpt-analytics ToxicityUpdate subscriber. CRTP-templated concrete
/// in `aeron::ToxicitySubscriber<H>` calls H::on_toxicity.

namespace bpt::bridge::messaging::api {

class ToxicitySubscriber {
public:
    virtual ~ToxicitySubscriber() = default;

    virtual int poll(int fragment_limit = 4) = 0;
};

}  // namespace bpt::bridge::messaging::api
