#pragma once

/// \file
/// \brief Shared FragmentHandler typedef for Aeron subscribers + decorators.
///
/// Lives in its own header so `chaos_filter.h` can wrap a handler
/// without depending on `subscriber.h` (which itself calls into the
/// chaos registry — would be a circular include otherwise).

#include <Aeron.h>

#include <functional>

namespace bpt::common::aeron {

using FragmentHandler =
    std::function<void(::aeron::AtomicBuffer&, ::aeron::util::index_t, ::aeron::util::index_t, ::aeron::Header&)>;

}  // namespace bpt::common::aeron
