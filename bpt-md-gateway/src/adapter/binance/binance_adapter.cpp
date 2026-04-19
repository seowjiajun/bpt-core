#include "md_gateway/adapter/binance/binance_adapter.h"

#include <messages/ExchangeId.h>

#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <cmath>
#include <cstring>
#include <simdjson.h>
#include <bpt_common/util/tsc_clock.h>
#include <bpt_common/ws/ws_connect.h>

namespace bpt::md_gateway::adapter {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;

BinanceAdapter::BinanceAdapter(const config::AdapterConfig& cfg, std::shared_ptr<messaging::IMdPublisher> md_pub)
    : AdapterBase(cfg, std::move(md_pub)),
      fr_ssl_ctx_(ssl::context::tls_client),
      parser_(subs_) {
    fr_ssl_ctx_.set_default_verify_paths();
    fr_ssl_ctx_.set_verify_mode(ssl::verify_peer);
    fr_ssl_ctx_.set_options(ssl::context::no_tlsv1 | ssl::context::no_tlsv1_1);
}

void BinanceAdapter::subscribe(uint64_t instrument_id, std::string symbol, uint8_t depth) {
    // Binance stream names are lowercase
    for (char& c : symbol)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    AdapterBase::subscribe(instrument_id, std::move(symbol), depth);
}

void BinanceAdapter::start() {
    AdapterBase::start();
    fr_thread_ = std::thread([this]() { run_funding_rate_loop(); });
}

void BinanceAdapter::stop() {
    AdapterBase::stop();
    fr_ioc_.stop();
    if (fr_thread_.joinable())
        fr_thread_.join();
}

std::unique_ptr<bpt::common::ws::AnyWsStream> BinanceAdapter::connect_and_subscribe() {
    std::string streams;
    for (const auto& [id, entry] : subs_.snapshot()) {
        if (!streams.empty())
            streams += '/';
        streams += entry.symbol + "@bookTicker/" + entry.symbol + "@aggTrade";
    }

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
                                      cfg_.ws_connect_timeout_ms);
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
    beast::flat_buffer buf;
    const auto liveness = std::chrono::milliseconds(cfg_.ws_liveness_timeout_ms);
    auto last_recv = std::chrono::steady_clock::now();

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        // Reset timer at the top of every iteration (consistent with other adapters).
        ws.expires_after(std::chrono::milliseconds(cfg_.ws_read_timeout_ms));
        beast::error_code ec;
        ws.read(buf, ec);

        if (ec == beast::error::timeout) {
            buf.consume(buf.size());
            if (std::chrono::steady_clock::now() - last_recv >= liveness) {
                bpt::common::log::warn("BinanceAdapter: no data for {}ms, reconnecting", cfg_.ws_liveness_timeout_ms);
                throw std::runtime_error("liveness timeout");
            }
            continue;
        }
        if (ec)
            throw beast::system_error(ec);

        last_recv = std::chrono::steady_clock::now();
        // WallClock, not TscClock — this timestamp crosses a process boundary
        // (bpt-md-gateway → fenrir via Aeron SBE) and would suffer from per-process
        // TscClock calibration drift. See HyperliquidAdapter for details.
        uint64_t recv_ns = bpt::common::util::WallClock::now_ns();
        push_frame(std::string_view(static_cast<const char*>(buf.data().data()), buf.data().size()), recv_ns);
        buf.consume(buf.size());
    }
    ws.close(websocket::close_code::normal);
}

void BinanceAdapter::parse_frame(std::string_view payload, uint64_t recv_ns) {
    parser_.parse(payload, recv_ns, validating_pub_, on_funding_rate);
}

// Connects to fstream.binance.com/stream?streams=!markPrice@arr@1s and publishes
// FundingRate updates for all subscribed instruments found in the stream.
void BinanceAdapter::run_funding_rate_loop() {
    const std::string fr_host = "fstream.binance.com";
    const std::string fr_port = "443";
    const std::string fr_path = "/stream?streams=!markPrice@arr@1s";

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        try {
            fr_ioc_.restart();
            auto ws = bpt::common::ws::ws_connect(fr_ioc_, fr_ssl_ctx_, fr_host, fr_port, fr_path, cfg_.so_rcvbuf_bytes);
            bpt::common::log::info("BinanceAdapter funding-rate stream connected");

            beast::flat_buffer buf;
            while (!stop_flag_.load(std::memory_order_relaxed)) {
                ws->read(buf);
                if (!on_funding_rate) {
                    buf.consume(buf.size());
                    continue;
                }

                uint64_t recv_ns = static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
                std::string_view sv(static_cast<const char*>(buf.data().data()), buf.data().size());
                buf.consume(buf.size());

                // Parse with simdjson on-demand — reused parser + padded buffer.
                if (fr_padded_buf_.size() < sv.size() + simdjson::SIMDJSON_PADDING)
                    fr_padded_buf_.resize(sv.size() + simdjson::SIMDJSON_PADDING);
                std::memcpy(fr_padded_buf_.data(), sv.data(), sv.size());
                std::memset(fr_padded_buf_.data() + sv.size(), 0, simdjson::SIMDJSON_PADDING);

                simdjson::ondemand::document doc;
                if (fr_json_parser_.iterate(fr_padded_buf_.data(), sv.size(), fr_padded_buf_.size()).get(doc))
                    continue;

                // {"stream":"!markPrice@arr","data":[{...},...]}
                simdjson::ondemand::array data_arr;
                if (doc.find_field_unordered("data").get_array().get(data_arr))
                    continue;

                for (auto item_res : data_arr) {
                    simdjson::ondemand::object entry;
                    if (item_res.get_object().get(entry))
                        continue;

                    std::string_view sym_sv;
                    if (entry["s"].get_string().get(sym_sv))
                        continue;

                    // Binance stream symbols are uppercase — lowercase for map lookup.
                    lower_sym_.assign(sym_sv);
                    for (char& c : lower_sym_)
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                    uint64_t instrument_id = subs_.find_id(lower_sym_);
                    if (!instrument_id)
                        continue;

                    double rate = 0.0;
                    (void)entry.find_field_unordered("r").get_double_in_string().get(rate);

                    uint64_t next_ms = 0;
                    (void)entry.find_field_unordered("T").get_uint64().get(next_ms);

                    messaging::FundingRateUpdate fr;
                    fr.instrument_id = instrument_id;
                    fr.exchange_id = bpt::messages::ExchangeId::BINANCE;
                    fr.rate_bps = static_cast<int32_t>(std::round(rate * 1'000'000.0));
                    fr.next_funding_ts_ns = next_ms * 1'000'000ULL;
                    fr.collected_ts_ns = recv_ns;
                    on_funding_rate(fr);
                }
            }

            ws->close(websocket::close_code::normal);
        } catch (const std::exception& e) {
            if (!stop_flag_.load(std::memory_order_relaxed)) {
                bpt::common::log::error("BinanceAdapter funding-rate error: {}, reconnecting in 5s", e.what());
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
        }
    }
}

}  // namespace bpt::md_gateway::adapter
