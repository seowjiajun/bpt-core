#pragma once

/// \file
/// \brief Aeron-fragment fault injection — drop a configurable fraction of inbound
/// fragments per stream-id. Used to validate graceful-degradation paths
/// (e.g. "if refdata stops, does the strategy keep trading on cached
/// snapshot?") without actually killing a service.
///
/// The seam is `bpt::common::aeron::Subscriber`'s ctor: every Subscriber
/// asks `ChaosRegistry::wrap(stream_id, handler)` before stashing the
/// handler in its FragmentAssembler. With no chaos configured the wrap
/// is identity — zero overhead, zero behaviour change.
///
/// Lifecycle:
///   1. Service `main()` parses `[chaos]` from TOML (see chaos_config.h).
///   2. `ChaosRegistry::set_global(cfg)` is called once before any
///      Subscriber is constructed.
///   3. From that point every Subscriber consults the registry by
///      stream_id at construction time. Polling is unaffected — the
///      drop check happens inside the wrapped handler, not on the hot
///      Aeron poll loop itself.
///
/// Prod safety: the loader (chaos_config.h) hard-throws if BPT_ENV=prod
/// and any drop_prob is configured, so the registry never sees a
/// production-affecting payload by construction.

#include "bpt_common/aeron/fragment_handler.h"

#include <Aeron.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <utility>

namespace bpt::common::aeron {

/// Drop-only chaos config: per-stream-id Bernoulli probability that an
/// incoming fragment is silently swallowed before reaching the user
/// handler. 0.0 = never drop (identity). 1.0 = drop everything (models
/// "stream is dead").
struct ChaosConfig {
    std::map<std::int32_t, double> drop_prob_by_stream_id;

    [[nodiscard]] bool empty() const noexcept { return drop_prob_by_stream_id.empty(); }
};

/// Process-global chaos registry. Set once from main() before any
/// Subscriber is constructed; consulted by every Subscriber ctor.
///
/// Thread-safety: `set_global` and `wrap` both take an internal mutex.
/// `wrap` is called on the cold path (subscriber construction), so the
/// lock is uncontended in practice. The hot path (Aeron poll → wrapped
/// handler) does NOT touch the registry — the rng + threshold are
/// captured by-value into the returned lambda.
class ChaosRegistry {
public:
    static void set_global(ChaosConfig cfg) {
        std::lock_guard lk(mu());
        config().emplace(std::move(cfg));
    }

    /// Return `inner` unchanged if no drop is configured for `stream_id`,
    /// otherwise wrap it in a Bernoulli-drop filter seeded from
    /// std::random_device. Each wrapped handler owns its own rng — no
    /// shared state on the poll path.
    static FragmentHandler wrap(std::int32_t stream_id, FragmentHandler inner) {
        double p = drop_prob_for(stream_id);
        if (p <= 0.0) {
            return inner;
        }
        return make_dropping_handler(std::move(inner), p);
    }

    /// Test-only: drop the global config so a subsequent set_global
    /// behaves as if the registry had never been touched.
    static void clear_for_testing() {
        std::lock_guard lk(mu());
        config().reset();
    }

    /// Test-only: peek the configured drop probability without going
    /// through wrap(). Returns 0.0 if no entry.
    static double drop_prob_for_testing(std::int32_t stream_id) { return drop_prob_for(stream_id); }

private:
    static std::optional<ChaosConfig>& config() {
        static std::optional<ChaosConfig> c;
        return c;
    }
    static std::mutex& mu() {
        static std::mutex m;
        return m;
    }

    static double drop_prob_for(std::int32_t stream_id) {
        std::lock_guard lk(mu());
        if (!config())
            return 0.0;
        const auto& m = config()->drop_prob_by_stream_id;
        auto it = m.find(stream_id);
        return it == m.end() ? 0.0 : it->second;
    }

    static FragmentHandler make_dropping_handler(FragmentHandler inner, double drop_prob) {
        // rng + dist captured by value; the lambda is mutable so dist's
        // non-const operator() compiles. std::function copies of this
        // handler will diverge in rng state — fine, we don't care about
        // cross-copy reproducibility.
        return [inner = std::move(inner),
                rng = std::mt19937_64{std::random_device{}()},
                dist = std::bernoulli_distribution{drop_prob}](::aeron::AtomicBuffer& buf,
                                                               ::aeron::util::index_t off,
                                                               ::aeron::util::index_t len,
                                                               ::aeron::Header& hdr) mutable {
            if (dist(rng)) {
                return;  // dropped
            }
            inner(buf, off, len, hdr);
        };
    }
};

}  // namespace bpt::common::aeron
