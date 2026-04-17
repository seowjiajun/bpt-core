#pragma once

// Persistent HTTPS/1.1 client to the Hyperliquid REST endpoint.
//
// Used by the adapter for `/info` queries (clearinghouseState, meta —
// unsigned public reads) and as a fallback for signed actions that the
// WS post path can't handle (notably `modify`, which HL rejects at WS
// parse time).
//
// Connection lifecycle: lazy connect on first call, reused across
// requests (HL honours keep-alive), reconnect-and-retry once on any
// I/O error. The TLS handshake is the dominant per-request cost without
// pooling — the retry counts as one full reconnect, not two.
//
// Thread-safety: every method holds an internal mutex for the duration
// of the call. Safe to call from the OrderProcessor thread + order-gateway's
// detached account-snapshot fetch thread concurrently.

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <memory>
#include <mutex>
#include <string>

namespace bpt::order_gateway::adapter::hyperliquid {

class HyperliquidHttpsClient {
public:
    // host/port are the REST endpoint — typically api.hyperliquid.xyz:443
    // or api.hyperliquid-testnet.xyz:443 from AdapterConfig.
    HyperliquidHttpsClient(std::string host, std::string port);

    // POST `body` to `path` with Content-Type: application/json, return
    // the response body. Throws std::exception on any I/O error that
    // survives the single retry.
    std::string post(const std::string& path, const std::string& body);

private:
    void connect();   // must be called with mutex_ held
    void close() noexcept;

    const std::string host_;
    const std::string port_;

    std::mutex mutex_;
    boost::asio::io_context ioc_;
    boost::asio::ssl::context ssl_ctx_{boost::asio::ssl::context::tls_client};
    std::unique_ptr<boost::beast::ssl_stream<boost::beast::tcp_stream>> stream_;
};

}  // namespace bpt::order_gateway::adapter::hyperliquid
