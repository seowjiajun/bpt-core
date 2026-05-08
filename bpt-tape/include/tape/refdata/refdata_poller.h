#pragma once

/// @file
/// Per-venue REST polling worker for bpt-tape. Owns one std::thread that
/// iterates a list of operator-declared endpoints (method/path/optional
/// request body/interval) and calls them via a RecordingRestClient — the
/// recording is a side effect of the call. Returned bodies are discarded:
/// the bytes are already on disk, and bpt-tape doesn't run a refdata
/// decoder.
///
/// Single thread per spool ⇒ RawSpool's single-writer invariant is held
/// without locks. Endpoints sharing the same (host, port, use_tls) tuple
/// share one client instance so we don't reload the SSL context per call.
///
/// Stop signalling is condvar-driven so `stop()` wakes the thread out of
/// its sleep immediately. An in-flight request can't be cancelled —
/// shutdown waits up to one HTTP timeout (~30s) for it to return.

#include "bpt_common/recorder/raw_spool.h"
#include "tape/http/recording_rest_client.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bpt::tape::refdata {

/// Operator-declared REST endpoint. Mirrors the shape of refdata's RestClient
/// call sites — host/port/use_tls so bpt-tape can construct the same client.
struct EndpointSpec {
    std::string exchange;          ///< "HYPERLIQUID" — used for spool venue tag
    std::string host;              ///< e.g. "api.hyperliquid.xyz"
    std::string port{"443"};
    bool use_tls{true};
    std::string method{"GET"};     ///< "GET" or "POST" (case-insensitive on parse)
    std::string path;              ///< e.g. "/info"
    std::string body;              ///< POST body (ignored for GET)
    uint32_t interval_seconds{3600};
};

/// Drives one venue's poll loop. Owns a spool, a thread, and one client
/// per (host,port,use_tls) tuple shared across the venue's endpoints.
class RefdataPoller {
public:
    RefdataPoller(std::string venue_tag,
                  std::shared_ptr<::bpt::common::recorder::RawSpool> spool,
                  std::vector<EndpointSpec> endpoints);

    ~RefdataPoller();

    RefdataPoller(const RefdataPoller&) = delete;
    RefdataPoller& operator=(const RefdataPoller&) = delete;

    /// Spawns the polling thread. Calls each endpoint once immediately, then
    /// re-fires per its `interval_seconds`. No-op if endpoints is empty.
    void start();

    /// Signals the poll thread to stop and joins it. Safe to call multiple
    /// times. An in-flight HTTP request runs to completion or its 30s
    /// timeout — that is the bound on shutdown latency.
    void stop();

private:
    struct EndpointState {
        EndpointSpec spec;
        std::shared_ptr<http::RecordingRestClient> client;
        std::chrono::steady_clock::time_point next_due{};
    };

    void run_loop();

    static uint64_t wall_now_ns();

    std::string venue_tag_;
    std::shared_ptr<::bpt::common::recorder::RawSpool> spool_;
    std::vector<EndpointState> endpoints_;

    std::thread thread_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
};

}  // namespace bpt::tape::refdata
