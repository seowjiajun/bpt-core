#pragma once

#include <Aeron.h>
#include <FragmentAssembler.h>

#include <messages/VolSurface.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace bpt::strategy::vol {

// Subscribes to Pricer's VolSurface (stream 4001) and status (stream 4002).
// Pure subscriber — no publications.  Single-threaded: only call from the poll thread.
class VolSurfaceClient {
public:
    using OnVolSurfaceFn = std::function<void(bpt::messages::VolSurface&)>;
    using OnReadyFn = std::function<void(uint8_t exchanges_loaded, uint16_t underlying_count, uint32_t point_count)>;

    VolSurfaceClient(std::shared_ptr<aeron::Aeron> aeron,
                     const std::string& channel,
                     int vol_surface_stream,
                     int pricer_status_stream,
                     int pub_timeout_ms = 5000,
                     int pub_poll_interval_ms = 10);

    int poll(int fragment_limit = 10);

    OnVolSurfaceFn on_vol_surface;
    OnReadyFn on_ready;

    [[nodiscard]] uint64_t last_heartbeat_ns() const { return last_heartbeat_ns_; }

private:
    void handle_surface_fragment(aeron::AtomicBuffer& buffer,
                                 aeron::util::index_t offset,
                                 aeron::util::index_t length,
                                 aeron::Header& header);

    void handle_status_fragment(aeron::AtomicBuffer& buffer,
                                aeron::util::index_t offset,
                                aeron::util::index_t length,
                                aeron::Header& header);

    std::shared_ptr<aeron::Subscription> surface_sub_;
    std::shared_ptr<aeron::Subscription> status_sub_;
    std::unique_ptr<aeron::FragmentAssembler> surface_assembler_;
    uint64_t last_heartbeat_ns_{0};
};

}  // namespace bpt::strategy::vol
