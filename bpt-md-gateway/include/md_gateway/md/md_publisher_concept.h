#pragma once

/// @file
/// Compile-time concepts for the MD publisher chain.
///
/// The tick path (venue decoder → ValidatingPublisher<Pub> → Pub) is
/// templated end-to-end to keep every `publish()` call statically
/// dispatched and inlinable. Without a concept, the contract `Pub`
/// must satisfy is buried in the templates' implementation — a typo
/// in a fake publisher only surfaces at instantiation site as a
/// long, hard-to-read template error. These concepts make the
/// contract first-class and give the compiler a name to use in
/// diagnostics.
///
/// Two layered concepts:
///
/// - `MdSink<P>` — minimum surface for anything the decoder pushes
///   into. Three `publish(...)` overloads. No drop_count. Tape
///   recorder, no-op replay sink, capturing test fake all satisfy
///   this without growing extra methods.
///
/// - `MdPublisher<P>` — `MdSink<P>` plus `drop_count()`. The
///   ValidatingPublisher decorator chains drops to its inner, so the
///   inner must expose that read. Prod `MdPublisher` (the class)
///   conforms; minimal sinks don't have to.

#include "md_gateway/md/md_types.h"

#include <concepts>
#include <cstdint>

namespace bpt::md_gateway::md {

/// A type P is an MdSink if a venue decoder can hand it the three
/// normalised market-data records the tick path produces.
template <class P>
concept MdSink = requires(P p, const MdBbo& bbo, const MdTrade& trade, const MdOrderBook& book) {
    p.publish(bbo);
    p.publish(trade);
    p.publish(book);
};

/// A type P is an MdPublisher if it's an MdSink and also exposes a
/// drop counter — used by ValidatingPublisher to layer its own drops
/// on top of the wrapped publisher's drops for the metrics reporter.
template <class P>
concept MdPublisher = MdSink<P> && requires(const P p) {
    { p.drop_count() } -> std::convertible_to<uint64_t>;
};

}  // namespace bpt::md_gateway::md
