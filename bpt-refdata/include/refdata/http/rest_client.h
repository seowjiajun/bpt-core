#pragma once

#include <boost/asio/ssl/context.hpp>
#include <string>
#include <utility>
#include <vector>

namespace bpt::refdata::http {

// Synchronous blocking REST client backed by Boost.Beast.
// Creates a fresh TCP connection per request but reuses the SSL context
// (avoids reloading CA certificates on every call).
// Retries up to 3 times with exponential backoff on transient errors;
// does not retry on HTTP 4xx.
// All methods throw std::runtime_error on non-2xx responses or connection failure.
//
// Methods are virtual to support recording-aware subclasses (used by
// bpt-md-recorder which wraps each instance with a tee that captures
// response bodies to disk). Adapters hold this via shared_ptr so a
// recorder process can substitute a subclass without recompiling refdata.
class RestClient {
public:
    using Headers = std::vector<std::pair<std::string, std::string>>;

    RestClient(std::string host, std::string port, bool use_tls);
    virtual ~RestClient() = default;

    virtual std::string get(const std::string& target, const Headers& extra_headers = {}) const;
    virtual std::string post(const std::string& target, const std::string& body, const Headers& extra_headers = {}) const;

protected:
    std::string host_;
    std::string port_;
    bool use_tls_;
    mutable boost::asio::ssl::context ssl_ctx_;  // Loaded once; reused across requests
};

}  // namespace bpt::refdata::http
