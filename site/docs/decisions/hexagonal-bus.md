# Hexagonal bus boundaries

**Choice:** every Aeron-using service exposes typed port interfaces in an
`api::` namespace; concrete Aeron bindings live in exactly one
`AeronBus::build()` factory per service.

**Main alternative considered:** direct Aeron coupling in app code (the way most
of the codebase used to be).

## The pattern

```cpp
// Port (in include/<service>/messaging/publishers/api/md_publisher.h):
namespace bpt::<svc>::messaging::api {
class MdPublisher {
public:
    virtual ~MdPublisher() = default;
    virtual void publish(const MdBbo&) = 0;
    virtual void publish(const MdTrade&) = 0;
    virtual void publish(const MdOrderBook&) = 0;
    virtual uint64_t drop_count() const = 0;
};
}

// Concrete (Aeron-backed, in messaging/publishers/aeron/md_publisher.h):
namespace bpt::<svc>::messaging::aeron {
class MdPublisher final : public api::MdPublisher { ... };
}

// Factory (sole place that #include <Aeron.h>):
struct ServiceBus {
    std::unique_ptr<api::MdControlSubscriber> control_source;
    std::shared_ptr<api::MdPublisher>         md_sink;
    std::unique_ptr<api::AckPublisher>        ack_sink;
    // ...
};

class ServiceAeronBus {
public:
    static ServiceBus build(std::shared_ptr<::aeron::Aeron>, const Settings&);
};

// App takes ports, never knows about Aeron:
class ServiceApp {
    ServiceApp(Settings, std::unique_ptr<api::MdControlSubscriber>,
               std::shared_ptr<api::MdPublisher>, ...);
};
```

The composition root (`main.cpp`) is the only file that knows both `Aeron` AND
`ServiceApp`:

```cpp
auto bus = ServiceAeronBus::build(ctx.aeron, cfg);
return std::make_unique<ServiceApp>(std::move(cfg), std::move(bus));
```

The folder + namespace structure (`api/`, `aeron/`, `sim/`) is itself a
documented decision; see [api-aeron-sim-layout.md](api-aeron-sim-layout.md)
for why the layout looks the way it does (no `I-` prefix, namespace
encodes the variant, folder mirrors the namespace).

## Why bother

1. **Test seam.** Component tests substitute a `FakeMdPublisher` / `FakeAckPublisher` that capture published frames in a vector — no Aeron in the test build. See `bpt-md-gateway/tests/component/fake_md_publisher.h`.
2. **Sim transport substitution.** The deterministic backtester (`bpt-backtester-mono`) can wire in `sim::*` concretes that skip SBE encode + Aeron offer entirely, dispatching domain objects via `std::function` directly. The port is what makes that swap possible without rewriting the app.
3. **Single chokepoint for cross-cutting concerns.** Chaos injection, metrics interception, sequence-gap detection — all hook into one place per service (the bus factory). Without the seam, every Aeron call site would need to know about the registry.
4. **Forward portability.** If shared-memory IPC ever changes (Chronicle Queue, custom ringbuffer, kernel-bypass like DPDK), only the `*AeronBus::build()` files change. The app, the strategy, the validator — none of them care.

## Why this isn't free

Virtual dispatch on `publish()` is one indirect call per message — measured
overhead is ~2-5 ns vs direct call. Acceptable on the cold path. **Not
acceptable on the publish hot path.**

That's why the hot path uses [CRTP / template composition instead](crtp-hot-path.md):
`decoder → ValidatingPublisher<MdPublisher> → MdPublisher` is fully inlined.
The hexagonal pattern lives at the bus seam (cold side); template composition
lives inside the publish chain (hot side). Both patterns coexist in
`bpt-md-gateway`, each in its right place.

## Where it landed

| Service | Pattern adoption |
|---|---|
| `bpt-refdata` | Full — 4 publishers + 1 subscriber, all api/aeron split |
| `bpt-md-gateway` | Full — 3 publishers + 1 subscriber api/aeron + MdPublisher CRTP hot path |
| `bpt-order-gateway` | Full — 3 publishers + 1 subscriber api/aeron |
| `bpt-strategy` | Full — 2 subscribers api/aeron + 1 publisher (PortfolioSnapshot) under `console/` |
| `bpt-pricer` | Full — 2 publishers api/aeron/sim + 3 subscribers api/aeron (md/aeron, md/api, refdata/aeron, refdata/api) |
| `bpt-analytics` | Full — 1 publisher + 2 subscribers api/aeron |
| `bpt-radar` | Full — 1 publisher + 6 subscribers api/aeron |
| `bpt-bridge` | Full — 1 publisher + 6 subscribers api/aeron |
| `bpt-pms` | Full — 1 publisher api/aeron |
| `bpt-backtester` | Full — 1 publisher + 1 subscriber api/aeron |

The pragmatic split — virtual ports where substitution matters (test fakes,
sim transports, bus-level chokepoints), template composition where the
publish chain is hot — is the right call. Forcing every concrete to a
virtual interface adds vtable cost for zero benefit; forcing every
publisher to a template adds compile-time and substitutability cost for
zero performance benefit on cold paths.

## See also

- [api-aeron-sim-layout.md](api-aeron-sim-layout.md) — the folder + namespace convention for ports.
- [crtp-hot-path.md](crtp-hot-path.md) — why the publish chain inside the bus uses template composition, not virtual dispatch.
- [`service-anatomy.md`](../../../docs/service-anatomy.md) — where the bus sits in the canonical layered service stack.
