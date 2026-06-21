#pragma once

/// \file
/// \brief OKX market-data adapter.

#include "md_gateway/adapter/common/adapter_base.h"
#include "md_gateway/adapter/okx/okx_md_decoder.h"
#include "md_gateway/adapter/okx/okx_md_encoder.h"
#include "md_gateway/adapter/okx/okx_md_ws_client.h"

#include <atomic>
#include <boost/asio/buffer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <bpt_common/logging.h>
#include <bpt_common/ws/ws_connect.h>
#include <chrono>
#include <memory>
#include <string>
#include <utility>

namespace bpt::md_gateway::adapter {

/// \brief Subscribes to OKX public WS, decodes frames, publishes SBE.
///
/// Connects to wss://ws.okx.com:8443/ws/v5/public. OKX requires
/// text-frame "ping" keepalives every 25 s — Beast's built-in control-
/// frame pings are disabled to prevent silent disconnects. Runtime
/// subscribe/unsubscribe take effect immediately via the pending queue.
///
/// Delegates read-loop mechanics (read timeout, ping thread, liveness
/// watchdog) to bpt::common::ws::RunLoop via OkxMdWsClient — the WS
/// client owns the venue protocol bits (ping/pong filter, on_tick
/// drain, ping_config) and forwards real frames here via handle_frame.
template <class Pub>
class OkxMdAdapter : public AdapterBase<Pub> {
public:
    using Base = AdapterBase<Pub>;

    explicit OkxMdAdapter(const config::AdapterConfig& cfg,
                          std::shared_ptr<Pub> md_pub,
                          std::shared_ptr<messaging::api::FundingRatePublisher> funding_pub,
                          std::shared_ptr<messaging::api::InstrumentStatsPublisher> stats_pub)
        : Base(cfg, std::move(md_pub), std::move(funding_pub), std::move(stats_pub)),
          decoder_(this->subs_),
          ws_client_(this->cfg_, this->subs_) {
        ws_client_.set_frame_handler([this](std::string_view p, uint64_t t) { this->handle_frame(p, t); });
    }

    [[nodiscard]] const char* exchange_name() const override { return "OKX"; }
    [[nodiscard]] bpt::common::util::LatencyHistogram& decode_latency_hist() noexcept override {
        return decoder_.decode_lat_;
    }

protected:
    /// \brief Send the OKX subscribe frame from the publisher-thread drain.
    ///
    /// Runs *after* subs_ has been updated, so the decoder is ready to
    /// translate the incoming BBO's symbol → instrument_id. ws_client_.send
    /// returns false when the WS isn't yet connected (between reconnects);
    /// in that case the frame is sent on next reconnect via subs_.snapshot().
    void do_send_subscribe_frame(std::string_view symbol, uint8_t depth) override {
        if (ws_client_.send(okx::build_subscribe_payload(std::string(symbol), depth))) {
            bpt::common::log::info("OkxMdAdapter: runtime subscribe {} depth={}", symbol, depth);
        }
    }

    std::unique_ptr<bpt::common::ws::AnyWsStream> connect_and_subscribe() override {
        namespace beast = boost::beast;
        namespace websocket = beast::websocket;
        namespace net = boost::asio;

        bpt::common::log::info("OkxMdAdapter connecting {}:{}{} (tls={})",
                               this->cfg_.ws_host,
                               this->cfg_.ws_port,
                               this->cfg_.ws_path,
                               this->cfg_.use_tls);

        std::unique_ptr<bpt::common::ws::AnyWsStream> any;
        if (this->cfg_.use_tls) {
            auto ws = bpt::common::ws::ws_connect(this->ioc_,
                                                  this->ssl_ctx_,
                                                  this->cfg_.ws_host,
                                                  this->cfg_.ws_port,
                                                  this->cfg_.ws_path,
                                                  this->cfg_.so_rcvbuf_bytes,
                                                  this->cfg_.ws_connect_timeout_ms,
                                                  "bpt-md-gateway/0.1",
                                                  this->cfg_.pinned_tls_sha256);
            any = std::make_unique<bpt::common::ws::AnyWsStream>(std::move(ws));
        } else {
            auto ws = bpt::common::ws::ws_connect_plain(this->ioc_,
                                                        this->cfg_.ws_host,
                                                        this->cfg_.ws_port,
                                                        this->cfg_.ws_path,
                                                        this->cfg_.so_rcvbuf_bytes,
                                                        this->cfg_.ws_connect_timeout_ms);
            any = std::make_unique<bpt::common::ws::AnyWsStream>(std::move(ws));
        }

        any->text(true);

        any->set_option(
            websocket::stream_base::timeout{websocket::stream_base::none(), std::chrono::seconds(60), false});

        bpt::common::log::info("OkxMdAdapter connected, subscribing instruments");

        this->subs_.take_pending();
        for (const auto& [id, entry] : this->subs_.snapshot())
            any->write(net::buffer(okx::build_subscribe_payload(entry.symbol, entry.depth)));

        return any;
    }

    void read_loop(bpt::common::ws::AnyWsStream& ws) override {
        ws_client_.run(std::move(ws),
                       this->ioc_,
                       this->stop_flag_,
                       rl_connected_,
                       std::chrono::milliseconds(this->cfg_.ws_read_timeout_ms),
                       std::chrono::milliseconds(this->cfg_.ws_liveness_timeout_ms));
    }

    void parse_frame(std::string_view payload, uint64_t recv_ns) override {
        decoder_.decode(payload, recv_ns, *this->md_pub_, this->on_funding_rate, this->on_instrument_stats);
    }

private:
    OkxMdDecoder<Pub> decoder_;
    OkxMdWsClient ws_client_;

    /// RunLoop::run signature needs a 'connected' atomic; AdapterBase
    /// already tracks connection state via on_connect/on_disconnect
    /// callbacks, so this flag is otherwise unused here.
    std::atomic<bool> rl_connected_{false};
};

}  // namespace bpt::md_gateway::adapter
