#pragma once

/// \file
/// \brief Subscribes to bpt-pricer's VolSurface stream (typically 4001).
///
/// Surface messages can span multiple Aeron fragments — we use FragmentAssembler
/// to reassemble. Mirror of bpt-strategy's VolSurfaceClient, scoped to radar's
/// needs (we only consume the surface stream; pricer-status heartbeat would be
/// a separate subscriber if/when we add liveness gating).

#include <Aeron.h>
#include <FragmentAssembler.h>

#include <messages/VolSurface.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace bpt::radar::messaging {

class VolSurfaceSubscriber {
public:
    using OnVolSurfaceFn = std::function<void(bpt::messages::VolSurface&)>;

    VolSurfaceSubscriber(std::shared_ptr<aeron::Aeron> aeron, const std::string& channel, int stream_id);

    /// \brief Poll for fragments; invokes on_vol_surface for each completed message.
    int poll(int fragment_limit = 4);

    OnVolSurfaceFn on_vol_surface;

private:
    void handle_fragment(aeron::AtomicBuffer& buffer,
                         aeron::util::index_t offset,
                         aeron::util::index_t length,
                         aeron::Header& header);

    std::shared_ptr<aeron::Subscription> sub_;
    std::unique_ptr<aeron::FragmentAssembler> assembler_;
};

}  // namespace bpt::radar::messaging
