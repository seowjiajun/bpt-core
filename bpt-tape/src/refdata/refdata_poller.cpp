#include "tape/refdata/refdata_poller.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <fmt/format.h>
#include <bpt_common/logging.h>

namespace bpt::tape::refdata {

namespace {

std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return s;
}

}  // namespace

uint64_t RefdataPoller::wall_now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

RefdataPoller::RefdataPoller(std::string venue_tag,
                             std::shared_ptr<::bpt::common::recorder::RawSpool> spool,
                             std::vector<EndpointSpec> endpoints)
    : venue_tag_(std::move(venue_tag)),
      spool_(std::move(spool)) {
    // One RecordingRestClient per (host,port,use_tls) tuple — shared across
    // endpoints that hit the same origin. Avoids reloading the SSL context
    // and CA bundle on every call.
    struct ClientKey {
        std::string host;
        std::string port;
        bool use_tls;
        bool operator==(const ClientKey& o) const {
            return host == o.host && port == o.port && use_tls == o.use_tls;
        }
    };
    std::vector<std::pair<ClientKey, std::shared_ptr<http::RecordingRestClient>>> clients;

    endpoints_.reserve(endpoints.size());
    for (auto& e : endpoints) {
        ClientKey key{e.host, e.port, e.use_tls};
        std::shared_ptr<http::RecordingRestClient> client;
        for (const auto& [k, c] : clients) {
            if (k == key) { client = c; break; }
        }
        if (!client) {
            client = std::make_shared<http::RecordingRestClient>(
                key.host, key.port, key.use_tls, spool_);
            clients.emplace_back(std::move(key), client);
        }
        endpoints_.push_back(EndpointState{std::move(e), std::move(client), {}});
    }
}

RefdataPoller::~RefdataPoller() {
    stop();
}

void RefdataPoller::start() {
    if (endpoints_.empty()) {
        bpt::common::log::info("bpt-tape: refdata poller [{}] no endpoints, skipping",
                               venue_tag_);
        return;
    }
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { run_loop(); });
    bpt::common::log::info("bpt-tape: refdata poller [{}] started ({} endpoints)",
                           venue_tag_, endpoints_.size());
}

void RefdataPoller::stop() {
    if (!running_.exchange(false))
        return;
    cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
    bpt::common::log::info("bpt-tape: refdata poller [{}] stopped", venue_tag_);
}

void RefdataPoller::run_loop() {
    using clock = std::chrono::steady_clock;
    const auto now = clock::now();
    for (auto& es : endpoints_)
        es.next_due = now;  // fire each endpoint immediately on startup

    while (running_.load(std::memory_order_acquire)) {
        const auto loop_start = clock::now();

        // Find the earliest-due endpoint at or before now and call it.
        // Endpoints are independent — call them one at a time so the spool
        // sees one writer.
        for (auto& es : endpoints_) {
            if (!running_.load(std::memory_order_acquire)) break;
            if (es.next_due > loop_start) continue;

            try {
                const std::string method = to_upper(es.spec.method);
                if (method == "GET") {
                    (void)es.client->get(es.spec.path);
                } else if (method == "POST") {
                    (void)es.client->post(es.spec.path, es.spec.body);
                } else {
                    bpt::common::log::warn(
                        "bpt-tape: refdata poller [{}] unknown method '{}' for {} — skipping",
                        venue_tag_, es.spec.method, es.spec.path);
                }
            } catch (const std::exception& e) {
                // RestClient already retried up to 3x with backoff; log and
                // move on to the next scheduled tick. Don't tear down the
                // poller for a single bad request.
                bpt::common::log::warn(
                    "bpt-tape: refdata poller [{}] {} {} failed: {}",
                    venue_tag_, es.spec.method, es.spec.path, e.what());
            }
            es.next_due = clock::now() +
                          std::chrono::seconds(es.spec.interval_seconds);
        }

        // Sleep until the soonest next_due, or until stop() pokes us.
        auto next = clock::time_point::max();
        for (const auto& es : endpoints_)
            next = std::min(next, es.next_due);
        if (next == clock::time_point::max())
            next = clock::now() + std::chrono::seconds(60);

        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait_until(lk, next, [this] {
            return !running_.load(std::memory_order_acquire);
        });
    }
}

}  // namespace bpt::tape::refdata
