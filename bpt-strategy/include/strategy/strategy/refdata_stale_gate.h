#pragma once

#include <cstdint>

namespace bpt::strategy::strategy {

// Tracks whether bpt-refdata is publishing heartbeats freshly enough.
//
// Strategies that read fee_cache / funding_rate_cache on the hot path
// (presently AS) need to know when refdata has gone silent — without it
// the caches return nullopt for stale entries and quotes ship with zero
// fee buffer, which is a silent bleed against taker fees on every fill.
//
// The gate exposes three state queries derived from a single bookkeeping
// pair (last_heartbeat_ns, started_at_ns):
//
//   evaluate(now_ns, last_heartbeat_ns) → State
//
// where State is one of:
//
//   Ok                    — heartbeat within threshold (or in startup window)
//   StartupTimedOut       — never received a heartbeat AND startup window expired
//   GoneStale             — was Ok, then heartbeat aged past stale_threshold_ns
//   Recovered             — was stale, then a fresh heartbeat arrived
//
// Reset / re-arm is implicit: a fresh heartbeat arriving after a stale
// period flips state from GoneStale → Recovered on the next evaluate(),
// then back to Ok on the one after.
//
// Header-only: simple state machine, no synchronization, called from
// strategy_app's main loop only (single thread).
//
// Not coupled to RefdataClient — caller passes in last_heartbeat_ns()
// and the current time. Keeps the gate trivially testable.
class RefdataStaleGate {
public:
    enum class State {
        Ok,
        StartupTimedOut,
        GoneStale,
        Recovered,
    };

    struct Config {
        // Time after process start without seeing any heartbeat that
        // counts as "refdata never came up." Default 60s — long enough
        // for cold-boot REST loads of fee schedules, short enough that
        // a misconfigured run dies before it sits silent for hours.
        uint64_t startup_timeout_ns{60'000'000'000ULL};

        // Time after the most recent heartbeat that counts as "refdata
        // went silent mid-session." Default 25s = 5× the 5s heartbeat
        // cadence in bpt-refdata/src/app/refdata_app.cpp. Three would
        // flap on transient publisher delays.
        uint64_t stale_threshold_ns{25'000'000'000ULL};
    };

    RefdataStaleGate() = default;
    explicit RefdataStaleGate(Config cfg) : cfg_(cfg) {}

    // Anchor for the startup window. Caller passes process-start
    // timestamp once; subsequent evaluate() calls reference it.
    void set_started_at(uint64_t now_ns) noexcept { started_at_ns_ = now_ns; }

    // Returns the new state and updates internal tracking. Call ~10Hz
    // from the main loop.
    State evaluate(uint64_t now_ns, uint64_t last_heartbeat_ns) noexcept {
        // Startup phase: never received a heartbeat yet.
        if (last_heartbeat_ns == 0) {
            if (started_at_ns_ != 0 && now_ns - started_at_ns_ > cfg_.startup_timeout_ns)
                return State::StartupTimedOut;
            return State::Ok;
        }

        const bool currently_stale = (now_ns - last_heartbeat_ns) > cfg_.stale_threshold_ns;

        if (currently_stale && !was_stale_) {
            was_stale_ = true;
            return State::GoneStale;
        }
        if (!currently_stale && was_stale_) {
            was_stale_ = false;
            return State::Recovered;
        }
        return currently_stale ? State::GoneStale : State::Ok;
    }

    [[nodiscard]] bool is_stale() const noexcept { return was_stale_; }

    [[nodiscard]] const Config& config() const noexcept { return cfg_; }

private:
    Config cfg_{};
    uint64_t started_at_ns_{0};
    bool was_stale_{false};
};

}  // namespace bpt::strategy::strategy
