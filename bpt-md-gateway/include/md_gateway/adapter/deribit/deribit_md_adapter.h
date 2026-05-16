#pragma once

/// \file
/// \brief Deribit market-data adapter (JSON-RPC 2.0 over WS).

#include "md_gateway/adapter/common/adapter_base.h"
#include "md_gateway/adapter/deribit/deribit_md_decoder.h"
#include "md_gateway/adapter/deribit/deribit_md_encoder.h"
#include "md_gateway/adapter/deribit/deribit_md_ws_client.h"
#include "md_gateway/md/validating_publisher.h"

#include <atomic>
#include <boost/asio/buffer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <bpt_common/logging.h>
#include <bpt_common/ws/ws_connect.h>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace bpt::md_gateway::adapter {

/// \brief Subscribes to Deribit public WS, decodes frames, publishes SBE.
///
/// Connects to wss://www.deribit.com/ws/api/v2 using JSON-RPC 2.0. Sends
/// public/set_heartbeat on connect; responds to incoming test_request
/// with public/test — Deribit disconnects within 30 s if this is missed.
/// Detects order-book gaps via prev_change_id; affected instruments are
/// automatically resubscribed.
///
/// Deribit's set_heartbeat keeps the session alive, so the WS client's
/// ping_config is left at nullopt — no application ping thread.
template <class Pub>
class DeribitMdAdapter : public AdapterBase<Pub> {
public:
    using Base = AdapterBase<Pub>;
    using ValidatingPub = md::ValidatingPublisher<Pub>;

    explicit DeribitMdAdapter(const config::AdapterConfig& cfg, std::shared_ptr<Pub> md_pub)
        : Base(cfg, std::move(md_pub)),
          decoder_(this->subs_),
          ws_client_(this->cfg_, this->subs_) {
        ws_client_.set_frame_handler([this](std::string_view p, uint64_t t) { this->handle_frame(p, t); });
    }

    /// \brief Unsubscribe + clear gap-detection state for the instrument in the decoder.
    void unsubscribe(uint64_t instrument_id) override {
        std::string symbol = this->subs_.unsubscribe(instrument_id);
        if (!symbol.empty())
            decoder_.forget(symbol);
    }

    /// \brief Push subscribe frames immediately on connect (bypassing on_tick).
    void subscribe(uint64_t instrument_id, std::string symbol, uint8_t depth = 0) override {
        Base::subscribe(instrument_id, symbol, depth);
        if (ws_client_.send(deribit::build_subscribe_rpc(ws_client_.next_rpc_id(), symbol, depth))) {
            bpt::common::log::info("DeribitMdAdapter: runtime subscribe {} depth={}", symbol, depth);
            this->subs_.take_pending();
        }
    }

    [[nodiscard]] const char* exchange_name() const override { return "DERIBIT"; }
    [[nodiscard]] bpt::common::util::LatencyHistogram& decode_latency_hist() noexcept override {
        return decoder_.decode_lat_;
    }

protected:
    /// \brief 2 s back-off — Deribit is slower to recover than the CEXs.
    std::chrono::milliseconds reconnect_delay() const override { return std::chrono::seconds(2); }

    std::unique_ptr<bpt::common::ws::AnyWsStream> connect_and_subscribe() override {
        namespace beast = boost::beast;
        namespace websocket = beast::websocket;
        namespace net = boost::asio;

        bpt::common::log::info("DeribitMdAdapter connecting {}:{}{}",
                               this->cfg_.ws_host,
                               this->cfg_.ws_port,
                               this->cfg_.ws_path);
        auto tls_ws = bpt::common::ws::ws_connect(this->ioc_,
                                                  this->ssl_ctx_,
                                                  this->cfg_.ws_host,
                                                  this->cfg_.ws_port,
                                                  this->cfg_.ws_path,
                                                  this->cfg_.so_rcvbuf_bytes,
                                                  this->cfg_.ws_connect_timeout_ms,
                                                  "bpt-md-gateway/0.1",
                                                  this->cfg_.pinned_tls_sha256);
        auto ws = std::make_unique<bpt::common::ws::AnyWsStream>(std::move(tls_ws));

        ws->text(true);
        ws->set_option(
            websocket::stream_base::timeout{websocket::stream_base::none(), websocket::stream_base::none(), false});

        bpt::common::log::info("DeribitMdAdapter connected");

        // Enable Deribit heartbeat — CRITICAL: Deribit disconnects within 30s if
        // test_request is not answered with public/test.
        ws->write(net::buffer(deribit::build_set_heartbeat_rpc(ws_client_.next_rpc_id(), /*interval_s=*/30)));
        bpt::common::log::info("DeribitMdAdapter: heartbeat enabled (interval=30s)");

        decoder_.reset();

        this->subs_.take_pending();
        for (const auto& [id, entry] : this->subs_.snapshot())
            ws->write(net::buffer(deribit::build_subscribe_rpc(ws_client_.next_rpc_id(), entry.symbol, entry.depth)));

        return ws;
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
        decoder_.decode(payload, recv_ns, this->validating_pub_, this->on_funding_rate, this->on_instrument_stats);
        if (decoder_.take_test_request())
            ws_client_.signal_test_request();
    }

private:
    DeribitMdDecoder<ValidatingPub> decoder_;
    DeribitMdWsClient ws_client_;
    std::atomic<bool> rl_connected_{false};
};

}  // namespace bpt::md_gateway::adapter
