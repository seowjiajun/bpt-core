#pragma once

/// \file
/// Port: VolSurface subscriber. CRTP-templated concrete in
/// `aeron::VolSurfaceSubscriber<H>`.

namespace bpt::radar::messaging::api {

class VolSurfaceSubscriber {
public:
    virtual ~VolSurfaceSubscriber() = default;

    virtual int poll(int fragment_limit = 4) = 0;
};

}  // namespace bpt::radar::messaging::api
