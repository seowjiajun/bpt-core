# C++ doc-comment style

Conventions for Doxygen-rendered comments in this codebase. Pragmatic
subset, biased toward LLVM-style. Apply as you touch files; no need to
sweep.

## Markers

- `///` for one-liner declaration comments and short doc blocks (preferred).
- `/** ... */` for multi-line blocks above a declaration.
- `///<` trailing comment on the same line as enum members / struct fields.
- Plain `//` for implementation notes inside function bodies (Doxygen
  ignores these — that's what we want).

## File headers

Every header gets a `\file` block stating *what the file contains and why
it exists*. Treat the second sentence as the most important one — it's
what someone reads when they're deciding whether this is the file they
want.

```cpp
#pragma once

/// \file
/// \brief Append-only raw-frame spool used by the recording host.
///
/// Captures venue payloads (WS frames from md-gateway adapters, REST
/// response bodies from refdata adapters) in their native bytes. Replay
/// through the backtester goes through the same parser code as live,
/// so any parser drift surfaces in test rather than production.
```

## Classes and functions

Every public class, function, and member gets a `\brief` line. One
sentence, ends with a period.

```cpp
/// \brief Subscribes to venue WS, decodes frames, publishes SBE on Aeron.
///
/// Owns the IO thread + publisher thread. Subclasses implement the
/// venue-specific connect_and_subscribe / read_loop / parse_frame.
class AdapterBase : public IAdapter { ... };
```

For functions, document params and return *only when non-obvious*. Don't
pad — if the parameter is clearly named and its type is self-explanatory,
skip it.

```cpp
/// \brief Append a raw venue frame.
/// \param recv_ts_ns wall-clock receive timestamp from on_frame
/// \param payload    bytes to write; not interpreted
/// \return false if the underlying file is closed or rotation failed
bool write_frame(uint64_t recv_ts_ns, std::string_view payload);
```

For functions where every parameter is self-evident, a single `\brief`
is enough:

```cpp
/// \brief Force-flush userspace buffer → stdio buffer → kernel.
void flush();
```

## Enums

`///<` trailing comments on enum members are the most readable form for
short descriptions:

```cpp
enum class RecordType : uint8_t {
    WS_FRAME      = 0,  ///< raw venue payload (JSON or FIX)
    SESSION_START = 1,  ///< recorder process started; payload = config snapshot
    SESSION_STOP  = 2,  ///< recorder process stopping cleanly
    CHECKPOINT    = 3,  ///< periodic heartbeat
};
```

## What to document

- *Why* this exists, not *what* it does — well-named identifiers handle
  the "what." Reserve doc-comments for non-obvious context, hidden
  invariants, gotchas, references to related design decisions.
- Threading invariants (which thread runs this; whether it's safe to
  call concurrently).
- Error semantics (exceptions thrown? returns null? logs and continues?).
- Surprising performance characteristics (hot path, blocking IO, lock
  acquisition).

## What to skip

- Trivial accessors. `int size() const` doesn't need a doc-block.
- Commit-message-shaped comments. "Added on 2024-04-15 to fix the OKX
  reconnect bug" belongs in `git log`, not the source.
- Repeating the function signature in prose. "Takes an int and returns
  a bool" reads worse than just letting the reader see the signature.

## Examples in the codebase

- `bpt-tape/include/tape/io/tape.h` — file header +
  per-method `\brief`.
- `bpt-app/include/bpt_app/app.h` — file header explaining the lifecycle
  contract.
- `bpt-md-gateway/include/md_gateway/adapter/common/adapter_base.h` —
  class-level docs for the framework + protected hooks.

When in doubt, look at how those files structure their comments and
follow the pattern.
