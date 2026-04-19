#pragma once

#include "pricer/surface/vol_surface_grid.h"

#include <Aeron.h>

#include <cstdint>
#include <memory>
#include <string>

namespace bpt::pricer::messaging {

class VolSurfacePublisher {
public:
    VolSurfacePublisher(std::shared_ptr<aeron::Aeron> aeron,
                        const std::string& channel,
                        int32_t stream_id);

    void publish(const surface::VolSurfaceGrid& grid, uint64_t timestamp_ns);

private:
    std::shared_ptr<aeron::Publication> pub_;
};

}  // namespace bpt::pricer::messaging
