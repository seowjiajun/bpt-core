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
class RestClient {
public:
    using Headers = std::vector<std::pair<std::string, std::string>>;

    RestClient(std::string host, std::string port, bool use_tls);

    std::string get(const std::string& target, const Headers& extra_headers = {}) const;
    std::string post(const std::string& target, const std::string& body, const Headers& extra_headers = {}) const;

private:
    std::string host_;
    std::string port_;
    bool use_tls_;
    mutable boost::asio::ssl::context ssl_ctx_;  // Loaded once; reused across requests
};

}  // namespace bpt::refdata::http
