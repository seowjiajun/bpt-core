#pragma once

// Binance user-data WebSocket client — owns the listenKey lifecycle
// and the read loop.
//
// Binance's user-data stream is keyed by a short-lived listenKey that
// must be created via REST (POST /api/v3/userDataStream), extended
// every 30 minutes (PUT), and deleted on clean shutdown (DELETE). The
// WS endpoint uses the listenKey as a path suffix — wss://.../ws/<key>.
//
// When the listenKey REST endpoint is unavailable (e.g. testnet 410
// error), run() falls back to REST-only mode: it parks on a 1s sleep
// loop and relies on the adapter's send_new_order REST-response path
// to emit ExecEvents. This preserves the pre-refactor behaviour.

#include "order_gateway/adapter/binance/binance_https_client.h"

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <cstdint>
#include <functional>
#include <string>

namespace bpt::order_gateway::adapter::binance {

class BinanceUserDataWs {
public:
    using MessageHandler = std::function<void(const std::string& payload, uint64_t recv_ns)>;

    BinanceUserDataWs(boost::asio::io_context& ioc,
                       boost::asio::ssl::context& ssl_ctx,
                       const config::AdapterConfig& cfg,
                       BinanceHttpsClient& https);

    void set_message_handler(MessageHandler h);

    // One connection session. Creates a listenKey, connects the WS,
    // runs the read loop until stop_flag is set, then deletes the
    // listenKey. Sets `connected` to true after the handshake; base
    // class clears it on return.
    //
    // If the listenKey endpoint returns empty (testnet 410 etc.), runs
    // in REST-only fallback: sleeps 1s in a loop with connected=true
    // and returns when stop_flag is set.
    void run(std::atomic<bool>& stop_flag, std::atomic<bool>& connected);

private:
    std::string create_listen_key();
    void extend_listen_key(const std::string& listen_key);
    void delete_listen_key(const std::string& listen_key);

    boost::asio::io_context& ioc_;
    boost::asio::ssl::context& ssl_ctx_;
    const config::AdapterConfig& cfg_;
    BinanceHttpsClient& https_;
    MessageHandler message_handler_;
};

}  // namespace bpt::order_gateway::adapter::binance
