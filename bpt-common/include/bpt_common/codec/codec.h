#pragma once

/// @file
/// Compile-time codec contracts. Concrete codecs (e.g. SbeVolSurfaceCodec,
/// SbeExecReportCodec) live with their owning service and assert conformance
/// via static_assert.
///
/// There's no virtual base class — the concept IS the interface, enforced at
/// compile time with zero runtime cost. In a Java codebase this would
/// typically be an `ICodec<T>` / `IEncoder<T>` / `IDecoder<T>` interface
/// hierarchy (cf. Millennium's mlp-net); the JIT erases the dispatch cost on
/// monomorphic call sites, so the design and the runtime cost are
/// independent. C++ doesn't have that JIT, so the right translation is
/// a `concept` — same enforcement, zero indirection.
///
/// Codecs that need extra context (timestamp not on the domain struct,
/// instrument cache, exchange-id resolver, etc.) take additional parameters
/// on their concrete signatures and only assert partial conformance (typically
/// just `Decoder<C, T>`). That's expected — the concept covers the minimum
/// round-trip surface every codec must provide, not a one-size-fits-all
/// signature for all codecs.

#include <concepts>
#include <cstddef>
#include <span>

namespace bpt::common::codec {

/// A type C is an Encoder<T> if it can serialise a T into a caller-supplied
/// scratch buffer and return the populated subspan, AND advertises a
/// worst-case scratch size as a compile-time constant. The kRecommendedScratchSize
/// requirement closes the "I forgot to declare the size" footgun — every
/// codec callsite needs the constant to size its stack buffer.
template <class C, class T>
concept Encoder = requires(C c, const T& obj, std::span<std::byte> scratch) {
    { c.encode(obj, scratch) } -> std::convertible_to<std::span<const std::byte>>;
    { C::kRecommendedScratchSize } -> std::convertible_to<std::size_t>;
};

/// A type C is a Decoder<T> if it can deserialise a byte span into a T.
template <class C, class T>
concept Decoder = requires(C c, std::span<const std::byte> bytes) {
    { c.decode(bytes) } -> std::convertible_to<T>;
};

/// A type C is a Codec<T> if it can both encode and decode T.
template <class C, class T>
concept Codec = Encoder<C, T> && Decoder<C, T>;

}  // namespace bpt::common::codec
