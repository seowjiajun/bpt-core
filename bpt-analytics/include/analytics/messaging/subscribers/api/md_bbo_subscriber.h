#pragma once

/// @file
/// Port: BBO subscriber. The per-frame dispatch path was lifted to a
/// CRTP-templated concrete subscriber — see
/// `aeron::MdBboSubscriber<Handler>` (Handler is `AnalyticsService` in
/// prod). Removing the `std::function` callback kills one indirection
/// per BBO tick.

namespace bpt::analytics::messaging::api {

class MdBboSubscriber {
public:
    virtual ~MdBboSubscriber() = default;

    virtual int poll(int fragment_limit = 10) = 0;
};

}  // namespace bpt::analytics::messaging::api
