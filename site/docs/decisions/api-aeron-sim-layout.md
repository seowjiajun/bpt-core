# Port layout — api / aeron / sim folder split

**Choice:** every port + concrete pair lives under three sibling folders —
`publishers/api/`, `publishers/aeron/`, `publishers/sim/` (and same shape under
`subscribers/`). The folder is the disambiguator; class names drop the
`I-` / `Aeron-` / `InProcess-` prefixes.

**Main alternative:** flat layout with prefixes —
`publishers/i_vol_surface_publisher.h`, `publishers/aeron_vol_surface_publisher.h`,
`publishers/in_process_vol_surface_publisher.h`. What the codebase used
before the 2026-05-17 rollout.

## The shape

```
bpt-pricer/include/pricer/messaging/publishers/
  api/    vol_surface_publisher.h     →  api::VolSurfacePublisher (pure virtual)
  api/    status_publisher.h          →  api::StatusPublisher
  aeron/  vol_surface_publisher.{h,cpp} → aeron::VolSurfacePublisher
  aeron/  status_publisher.{h,cpp}      → aeron::StatusPublisher
  sim/    vol_surface_publisher.h       → sim::VolSurfacePublisher
  codecs/ sbe_vol_surface_codec.h       → SbeVolSurfaceCodec (unchanged)
```

Class names short; namespace path encodes the variant. Same filename
across folders is a feature — `grep -r vol_surface_publisher.h` shows
every transport binding at once.

## Why

**1. Folder is the right disambiguator when there are >2 transports per port.**

A flat layout with prefixes works for two implementations. The moment you
add a sim variant or a test fake, you have three or four files all starting
with different prefixes and the folder becomes hard to scan. Sorting all
three of `i_`, `aeron_`, `sim_` together loses the structure. With api/aeron/sim
folders, you can list one folder and see all the ports, or one folder and see
all the Aeron concretes — sharper mental model.

**2. Class names should read naturally at the use site.**

After the split:
```cpp
std::unique_ptr<api::VolSurfacePublisher> vol_pub_;
bus.vol_pub = std::make_unique<aeron::VolSurfacePublisher>(...);
```

Before the split:
```cpp
std::unique_ptr<IVolSurfacePublisher> vol_pub_;
bus.vol_pub = std::make_unique<AeronVolSurfacePublisher>(...);
```

Both work. The split reads cleaner at multiple use sites because the
short class name `VolSurfacePublisher` is the same on both sides — the
namespace varies. Matches modern C++ idiom (no Hungarian-style I-prefix).

**3. Symmetric with the codec convention already in place.**

`messaging/codecs/sbe_vol_surface_codec.h` lives next to publishers; the
codec is the third axis (encoding). Having `api/aeron/sim/codecs/` as
four parallel siblings is clean. Hungarian prefixes on the publishers
broke that symmetry.

**4. Future-proofs the sim path.**

Adding a sim concrete is now `cp aeron/foo.h sim/foo.h` and rewriting the
namespace — same path everywhere, no rename collision with existing files.
Before the split, adding a sim variant meant inventing yet another prefix.

## The `::aeron::` qualification cost

The choice has one cost: our inner namespace `bpt::<svc>::messaging::aeron`
clashes with the upstream library's global `::aeron` namespace. Inside our
`aeron::` namespace, `aeron::Aeron` resolves to (nonexistent)
`bpt::<svc>::messaging::aeron::Aeron`, not the upstream `::aeron::Aeron`.

We pay this with explicit `::aeron::` qualification inside our `aeron::`
concrete files:

```cpp
namespace bpt::pricer::messaging::aeron {
    VolSurfacePublisher::VolSurfacePublisher(std::shared_ptr<::aeron::Aeron> aeron, ...)
                                                            // ^^ explicit global
    {
        pub_ = bpt::common::aeron::wait_for_publication(aeron, channel, stream_id);
    }

    void VolSurfacePublisher::publish(...) {
        ::aeron::concurrent::AtomicBuffer buffer(...);  // ^^ explicit global
        pub_->offer(buffer, 0, bytes.size());
    }
}
```

Typical concrete file needs 3-5 such explicit qualifications. Tolerable
cost; concentrated in files that already exist to deal with Aeron
specifics.

## What this is *not*

This is a **layout** decision, not a **dispatch** decision. The port
pattern itself (virtual ports as the bus boundary) was already in place
from the [hexagonal bus](hexagonal-bus.md) decision. This ADR documents
how the ports are organized in the filesystem and namespace, not whether
to have them.

The companion choice of **template composition + concepts** for the hot
path (where vtable cost matters) lives at
[crtp-hot-path.md](crtp-hot-path.md). That stays orthogonal: hot path
templates do not get the api/aeron/sim split because there's no runtime
substitution — `MdPublisher` is one concrete class, used directly.

## When this layout doesn't earn its keep

A service whose only publisher has one transport (Aeron, no sim, no test
fake) gains nothing from the split. The folder structure adds two empty
folders. Honest verdict: about 4-5 of the ~30 ports currently in the
codebase fall into this category. We applied the convention uniformly for
consistency rather than per-port — the cost of three empty folders is
~0; the cost of an inconsistent layout across 8 services is real.

If a port genuinely never grows a second concrete, the api/ folder is
dead weight forever. Acceptable.

## Naming inside subscribers

For inbound ports, the same rule applies but the names follow the
Aeron-concrete naming, not the hexagonal-DDD "Source/Sink" vocabulary:

- `api::MdControlSubscriber` (not `MdControlSource`)
- `api::OrderSubscriber` (not `OrderControlSource`)
- `api::RefdataControlSubscriber` (not `RefdataControlSource`)

Source/Sink read fine in hexagonal-architecture papers but split the
codebase's mental model in half — half the team calls it a sink, the
other half a subscriber. Pick one. We picked "Subscriber" because that's
what the concrete is called and what Aeron calls it.

## See also

- [hexagonal-bus.md](hexagonal-bus.md) — the original port-at-bus decision.
- [crtp-hot-path.md](crtp-hot-path.md) — why the hot path doesn't follow this layout (templates, not ports).
- [`service-anatomy.md`](../../../docs/service-anatomy.md) — where ports + concretes + codecs fit in the service layer stack.
