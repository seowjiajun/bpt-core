#pragma once

/// \file
/// \brief Binance market-data adapter (book + trades + funding rate).

#include "md_gateway/adapter/binance/binance_funding_rate_stream.h"
#include "md_gateway/adapter/binance/binance_md_decoder.h"
#include "md_gateway/adapter/binance/binance_md_encoder.h"
#include "md_gateway/adapter/binance/binance_md_ws_client.h"
#include "md_gateway/adapter/common/adapter_base.h"
#include "md_gateway/md/validating_publisher.h"

#include <atomic>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <bpt_common/logging.h>
#include <bpt_common/ws/ws_connect.h>
#include <cctype>
#include <chrono>
#include <memory>
#include <string>
#include <utility>

namespace bpt::md_gateway::adapter {

/// \brief Subscribes to Binance public WS, decodes frames, publishes SBE.
///
/// Two parallel streams:
///   - Main WS at stream.binance.com:9443 — subscriptions are baked
///     into the URL (`/stream?streams=<sym>@bookTicker/<sym>@aggTrade/...`),
///     so runtime subscribe / unsubscribe only take effect on the next
///     reconnect.
///   - Funding-rate WS (`fstream.binance.com/stream?streams=!markPrice@arr@1s`)
///     runs on its own thread inside BinanceFundingRateStream — Binance
///     hosts funding/mark on a separate global broadcast endpoint, so
///     it can't ride on the per-symbol main WS.
///
/// Uses Beast's WS-level control-frame pings (configured in
/// connect_and_subscribe) — the ws-client's ping_config is left at
/// nullopt; no application ping thread.
template <class Pub>
class BinanceMdAdapter : public AdapterBase<Pub> {
public:
    using Base = AdapterBase<Pub>;
    using ValidatingPub = md::ValidatingPublisher<Pub>;

    explicit BinanceMdAdapter(const config::AdapterConfig& cfg, std::shared_ptr<Pub> md_pub)
        : Base(cfg, std::move(md_pub)),
          decoder_(this->subs_),
          fr_stream_(this->cfg_, this->subs_, this->on_funding_rate, this->stop_flag_) {
        ws_client_.set_frame_handler([this](std::string_view p, uint64_t t) { this->handle_frame(p, t); });
    }

    /// \brief Register a subscription. Lowercases the symbol — Binance stream names are lowercase.
    void subscribe(uint64_t instrument_id, std::string symbol, uint8_t depth = 0) override {
        for (char& c : symbol)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        Base::subscribe(instrument_id, std::move(symbol), depth);
    }

    /// \brief Start the main IO + publisher threads AND the funding-rate stream.
    void start() override {
        Base::start();
        fr_stream_.start();
    }

    /// \brief Stop the main threads AND the funding-rate stream.
    void stop() override {
        Base::stop();
        fr_stream_.stop();
    }

    [[nodiscard]] const char* exchange_name() const override { return "BINANCE"; }
    [[nodiscard]] bpt::common::util::LatencyHistogram& decode_latency_hist() noexcept override {
        return decoder_.decode_lat_;
    }

protected:
    std::unique_ptr<bpt::common::ws::AnyWsStream> connect_and_subscribe() override {
        namespace beast = boost::beast;
        namespace websocket = beast::websocket;

        std::string streams = binance::build_streams_query(this->subs_);
        if (streams.empty())
            return nullptr;

        const std::string path = this->cfg_.ws_path + "?streams=" + streams;
        bpt::common::log::info("BinanceMdAdapter connecting {}:{}{}", this->cfg_.ws_host, this->cfg_.ws_port, path);
        auto tls_ws = bpt::common::ws::ws_connect(this->ioc_,
                                                  this->ssl_ctx_,
                                                  this->cfg_.ws_host,
                                                  this->cfg_.ws_port,
                                                  path,
                                                  this->cfg_.so_rcvbuf_bytes,
                                                  this->cfg_.ws_connect_timeout_ms,
                                                  "bpt-md-gateway/0.1",
                                                  this->cfg_.pinned_tls_sha256);
        auto ws = std::make_unique<bpt::common::ws::AnyWsStream>(std::move(tls_ws));

        // WS-level keep-alive pings; reconnect loop catches close-on-no-ping.
        ws->set_option(websocket::stream_base::timeout{websocket::stream_base::none(), std::chrono::seconds(30), true});

        bpt::common::log::info("BinanceMdAdapter connected");
        return ws;
    }

    void read_loop(bpt::common::ws::AnyWsStream& ws) override {
        ws_client_.run(std::move(ws),
                       this->stop_flag_,
                       rl_connected_,
                       std::chrono::milliseconds(this->cfg_.ws_read_timeout_ms),
                       std::chrono::milliseconds(this->cfg_.ws_liveness_timeout_ms));
    }

    void parse_frame(std::string_view payload, uint64_t recv_ns) override {
        decoder_.decode(payload, recv_ns, this->validating_pub_, this->on_funding_rate);
    }

private:
    BinanceMdDecoder<ValidatingPub> decoder_;
    BinanceMdWsClient ws_client_;
    /// RunLoop::run signature needs a 'connected' atomic; AdapterBase
    /// already tracks connection state via on_connect/on_disconnect
    /// callbacks, so this flag is otherwise unused here.
    std::atomic<bool> rl_connected_{false};
    BinanceFundingRateStream fr_stream_;
};

}  // namespace bpt::md_gateway::adapter
