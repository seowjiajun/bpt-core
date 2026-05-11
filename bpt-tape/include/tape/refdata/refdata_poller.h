#pragma once

/// \file
/// \brief Per-venue REST polling worker — records refdata response bodies.
///
/// Owns one std::thread that iterates operator-declared endpoints and
/// calls them via a RecordingRestClient — recording is a side effect of
/// the call. Returned bodies are discarded; bpt-tape doesn't decode
/// refdata, the bytes are already on disk.
///
/// Threading: single thread per tape keeps Tape's single-writer
/// invariant without locks. Endpoints sharing the same
/// (host, port, use_tls) tuple share one client instance — avoids
/// reloading the SSL context per call.
///
/// Shutdown: condvar-driven so stop() wakes the thread out of sleep
/// immediately. An in-flight request can't be cancelled; shutdown
/// latency is bounded by the HTTP timeout (~30 s).

#include "tape/io/tape.h"
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

/// \brief Operator-declared REST endpoint to poll + record.
///
/// Mirrors refdata's RestClient call-site shape (host/port/use_tls)
/// so bpt-tape can construct the same client without depending on
/// the refdata service's adapter code.
struct EndpointSpec {
    std::string exchange;            ///< venue tag, picks the tape
    std::string host;                ///< e.g. "api.hyperliquid.xyz"
    std::string port{"443"};
    bool use_tls{true};
    std::string method{"GET"};       ///< "GET" or "POST" (case-insensitive)
    std::string path;                ///< e.g. "/info"
    std::string body;                ///< POST body (ignored for GET)
    uint32_t interval_seconds{3600}; ///< poll cadence
};

/// \brief Drives one venue's poll loop.
///
/// Owns the tape, a single worker thread, and one client per
/// (host,port,use_tls) tuple shared across endpoints. Lifecycle:
/// start() spawns, stop() joins, destructor calls stop() defensively.
class RefdataPoller {
public:
    RefdataPoller(std::string venue_tag,
                  std::shared_ptr<::bpt::tape::io::Tape> tape,
                  std::vector<EndpointSpec> endpoints);

    ~RefdataPoller();

    RefdataPoller(const RefdataPoller&) = delete;
    RefdataPoller& operator=(const RefdataPoller&) = delete;

    /// \brief Spawn the polling thread. Fires every endpoint once
    ///        immediately, then re-fires per `interval_seconds`.
    ///        No-op if the endpoint list is empty.
    void start();

    /// \brief Signal the thread to stop and join. Idempotent.
    ///
    /// An in-flight HTTP request runs to completion or its ~30 s
    /// timeout — that is the bound on shutdown latency.
    void stop();

private:
    struct EndpointState {
        EndpointSpec spec;
        std::shared_ptr<http::RecordingRestClient> client;
        std::chrono::steady_clock::time_point next_due{};
    };

    void run_loop();

    std::string venue_tag_;
    std::shared_ptr<::bpt::tape::io::Tape> tape_;
    std::vector<EndpointState> endpoints_;

    std::thread thread_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
};

}  // namespace bpt::tape::refdata
