#include "md_gateway/adapter/binance/binance_adapter.h"

#include "md_gateway/adapter/binance/binance_md_encoder.h"

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <bpt_common/ws/ws_connect.h>

namespace bpt::md_gateway::adapter {

namespace beast = boost::beast;
namespace websocket = beast::websocket;

BinanceAdapter::BinanceAdapter(const config::AdapterConfig& cfg, std::shared_ptr<messaging::IMdPublisher> md_pub, const config::RecordingConfig& recording)
    : AdapterBase(cfg, std::move(md_pub), recording),
      parser_(subs_),
      fr_stream_(cfg_, subs_, on_funding_rate, stop_flag_) {}

void BinanceAdapter::subscribe(uint64_t instrument_id, std::string symbol, uint8_t depth) {
    // Binance stream names are lowercase
    for (char& c : symbol)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    AdapterBase::subscribe(instrument_id, std::move(symbol), depth);
}

void BinanceAdapter::start() {
    AdapterBase::start();
    fr_stream_.start();
}

void BinanceAdapter::stop() {
    AdapterBase::stop();
    fr_stream_.stop();
}

std::unique_ptr<bpt::common::ws::AnyWsStream> BinanceAdapter::connect_and_subscribe() {
    std::string streams = binance::build_streams_query(subs_);
    if (streams.empty())
        return nullptr;

    const std::string path = cfg_.ws_path + "?streams=" + streams;
    bpt::common::log::info("BinanceAdapter connecting {}:{}{}", cfg_.ws_host, cfg_.ws_port, path);
    auto tls_ws = bpt::common::ws::ws_connect(ioc_,
                                      ssl_ctx_,
                                      cfg_.ws_host,
                                      cfg_.ws_port,
                                      path,
                                      cfg_.so_rcvbuf_bytes,
                                      cfg_.ws_connect_timeout_ms,
                                      "bpt-md-gateway/0.1",
                                      cfg_.pinned_tls_sha256);
    auto ws = std::make_unique<bpt::common::ws::AnyWsStream>(std::move(tls_ws));

    // Enable WebSocket-level keep-alive pings. If Binance stops responding
    // Beast closes the stream with an error, triggering the reconnect loop.
    // Complements the application-level last_recv liveness check in read_loop.
    ws->set_option(websocket::stream_base::timeout{
        websocket::stream_base::none(),  // connect timeout handled in ws_connect
        std::chrono::seconds(30),        // idle timeout before Beast sends a ping
        true                             // send keep-alive ping frames
    });

    bpt::common::log::info("BinanceAdapter connected");
    return ws;
}

void BinanceAdapter::read_loop(bpt::common::ws::AnyWsStream& ws) {
    RunLoop::run(std::move(ws),
                 stop_flag_,
                 rl_connected_,
                 std::chrono::milliseconds(cfg_.ws_read_timeout_ms),
                 std::chrono::milliseconds(cfg_.ws_liveness_timeout_ms));
}

void BinanceAdapter::on_frame(std::string_view payload, uint64_t recv_ns) {
    record_raw(payload, recv_ns);
    push_frame(payload, recv_ns);
}

void BinanceAdapter::parse_frame(std::string_view payload, uint64_t recv_ns) {
    parser_.parse(payload, recv_ns, validating_pub_, on_funding_rate);
}

}  // namespace bpt::md_gateway::adapter
