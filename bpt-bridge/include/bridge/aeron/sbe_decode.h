#pragma once

/// \file
/// \brief Templated SBE-fragment decode helper shared across bridge subscribers.
///
/// Centralises the 5-line preamble that every Aeron-fragment handler
/// repeats: length sanity check, MessageHeader wrap, templateId match,
/// wrapForDecode on the flyweight, then invoke the caller's lambda with
/// the decoded message reference.
///
/// Lives in the bridge for now — the same pattern appears in
/// strategy/aeron_order_gateway_client.cpp, vol_surface_client.cpp, and
/// backtest_client.cpp. Lift to bpt-common/aeron/ if we sweep those too.

#include <Aeron.h>

#include <messages/MessageHeader.h>

#include <utility>

namespace bpt::bridge {

template <class SbeMessage, class F>
inline void decode_sbe_fragment(::aeron::AtomicBuffer& buffer,
                                ::aeron::util::index_t offset,
                                ::aeron::util::index_t length,
                                F&& on_message) {
    using ::bpt::messages::MessageHeader;
    if (length < static_cast<::aeron::util::index_t>(MessageHeader::encodedLength()))
        return;
    auto* data = reinterpret_cast<char*>(buffer.buffer() + offset);
    MessageHeader hdr;
    hdr.wrap(data, 0, MessageHeader::sbeSchemaVersion(), static_cast<uint64_t>(length));
    if (hdr.templateId() != SbeMessage::sbeTemplateId())
        return;
    SbeMessage msg;
    msg.wrapForDecode(data,
                      MessageHeader::encodedLength(),
                      hdr.blockLength(),
                      hdr.version(),
                      static_cast<uint64_t>(length));
    std::forward<F>(on_message)(msg);
}

}  // namespace bpt::bridge
