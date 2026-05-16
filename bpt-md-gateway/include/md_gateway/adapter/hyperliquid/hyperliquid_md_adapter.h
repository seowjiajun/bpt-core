#pragma once

/// \file
/// \brief Hyperliquid market-data adapter.

#include "md_gateway/adapter/common/adapter_base.h"
#include "md_gateway/adapter/hyperliquid/hyperliquid_md_decoder.h"
#include "md_gateway/adapter/hyperliquid/hyperliquid_md_encoder.h"
#include "md_gateway/adapter/hyperliquid/hyperliquid_md_ws_client.h"
#include "md_gateway/md/validating_publisher.h"

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

/// \brief Subscribes to Hyperliquid public WS, decodes frames, publishes SBE.
///
/// Connects to wss://api.hyperliquid.xyz/ws. Subscribes to l2Book,
/// trades, and activeAssetCtx per instrument. Runtime subscribe /
/// unsubscribe take effect immediately via the pending queue.
///
/// Both perp and spot instruments route through the same subscribe path.
/// HL's WS accepts the pair name verbatim as the `coin` field
/// ("PURR/USDC" for spot, "BTC" for perp) and mirrors that name in
/// response frames — no `@N` translation needed. The `@N` universe-index
/// form is *not* accepted by the WS endpoint (server-side reject).
///
/// activeAssetCtx behaves differently for spot: HL responds with
/// `channel:"activeSpotAssetCtx"` (different schema, no funding rate).
/// The decoder only branches on the perp `activeAssetCtx` channel, so
/// spot frames silently fall through — correct, because spot has no
/// funding to extract.
///
/// HL closes idle WebSockets ~60 s after the last client-sent message,
/// so the WS client's ping_config emits a JSON `{"method":"ping"}`
/// payload on a 20 s cadence (control-frame pings don't reset HL's
/// idle timer).
template <class Pub>
class HyperliquidMdAdapter : public AdapterBase<Pub> {
public:
    using Base = AdapterBase<Pub>;
    using ValidatingPub = md::ValidatingPublisher<Pub>;

    explicit HyperliquidMdAdapter(const config::AdapterConfig& cfg, std::shared_ptr<Pub> md_pub)
        : Base(cfg, std::move(md_pub)),
          decoder_(this->subs_),
          ws_client_(this->cfg_, this->subs_) {
        ws_client_.set_frame_handler([this](std::string_view p, uint64_t t) { this->handle_frame(p, t); });
    }

    [[nodiscard]] const char* exchange_name() const override { return "HYPERLIQUID"; }
    [[nodiscard]] bpt::common::util::LatencyHistogram& decode_latency_hist() noexcept override {
        return decoder_.decode_lat_;
    }

    /// \brief Push the 3 subscribe frames (l2Book, trades, activeAssetCtx) immediately on connect.
    void subscribe(uint64_t instrument_id, std::string symbol, uint8_t depth = 0) override {
        Base::subscribe(instrument_id, symbol, depth);
        bool sent = false;
        for (const char* type : {"l2Book", "trades", "activeAssetCtx"}) {
            if (ws_client_.send(hyperliquid::build_subscribe_payload(type, symbol)))
                sent = true;
        }
        if (sent) {
            bpt::common::log::info("HyperliquidMdAdapter: runtime subscribe {}", symbol);
            this->subs_.take_pending();
        }
    }

protected:
    std::unique_ptr<bpt::common::ws::AnyWsStream> connect_and_subscribe() override {
        namespace beast = boost::beast;
        namespace websocket = beast::websocket;
        namespace net = boost::asio;

        bpt::common::log::info("HyperliquidMdAdapter connecting {}:{}{} (tls={})",
                               this->cfg_.ws_host,
                               this->cfg_.ws_port,
                               this->cfg_.ws_path,
                               this->cfg_.use_tls);
        std::unique_ptr<bpt::common::ws::AnyWsStream> ws;
        if (this->cfg_.use_tls) {
            auto tls_ws = bpt::common::ws::ws_connect(this->ioc_,
                                                      this->ssl_ctx_,
                                                      this->cfg_.ws_host,
                                                      this->cfg_.ws_port,
                                                      this->cfg_.ws_path,
                                                      this->cfg_.so_rcvbuf_bytes,
                                                      this->cfg_.ws_connect_timeout_ms,
                                                      "bpt-md-gateway/0.1",
                                                      this->cfg_.pinned_tls_sha256);
            ws = std::make_unique<bpt::common::ws::AnyWsStream>(std::move(tls_ws));
        } else {
            auto plain_ws = bpt::common::ws::ws_connect_plain(this->ioc_,
                                                              this->cfg_.ws_host,
                                                              this->cfg_.ws_port,
                                                              this->cfg_.ws_path,
                                                              this->cfg_.so_rcvbuf_bytes,
                                                              this->cfg_.ws_connect_timeout_ms);
            ws = std::make_unique<bpt::common::ws::AnyWsStream>(std::move(plain_ws));
        }

        ws->set_option(websocket::stream_base::timeout{websocket::stream_base::none(), std::chrono::seconds(30), true});

        bpt::common::log::info("HyperliquidMdAdapter connected, subscribing instruments");

        this->subs_.take_pending();
        for (const auto& [id, entry] : this->subs_.snapshot()) {
            for (const char* type : {"l2Book", "trades", "activeAssetCtx"})
                ws->write(net::buffer(hyperliquid::build_subscribe_payload(type, entry.symbol)));
        }

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
    }

private:
    HyperliquidMdDecoder<ValidatingPub> decoder_;
    HyperliquidMdWsClient ws_client_;
    std::atomic<bool> rl_connected_{false};
};

}  // namespace bpt::md_gateway::adapter
