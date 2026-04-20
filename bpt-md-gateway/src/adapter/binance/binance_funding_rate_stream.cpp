#include "md_gateway/adapter/binance/binance_funding_rate_stream.h"

#include <messages/ExchangeId.h>

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <cmath>
#include <cstring>
#include <bpt_common/logging.h>
#include <bpt_common/util/thread_name.h>
#include <bpt_common/ws/ws_connect.h>

namespace bpt::md_gateway::adapter {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace ssl = boost::asio::ssl;

BinanceFundingRateStream::BinanceFundingRateStream(const config::AdapterConfig& cfg,
                                                    const SubscriptionMap& subs,
                                                    messaging::FundingRateCallback& on_funding_rate,
                                                    std::atomic<bool>& stop_flag)
    : cfg_(cfg),
      subs_(subs),
      on_funding_rate_(on_funding_rate),
      stop_flag_(stop_flag),
      ssl_ctx_(ssl::context::tls_client) {
    ssl_ctx_.set_default_verify_paths();
    ssl_ctx_.set_verify_mode(ssl::verify_peer);
    ssl_ctx_.set_options(ssl::context::no_tlsv1 | ssl::context::no_tlsv1_1);
}

void BinanceFundingRateStream::start() {
    thread_ = std::thread([this]() { run(); });
}

void BinanceFundingRateStream::stop() {
    ioc_.stop();
    if (thread_.joinable())
        thread_.join();
}

void BinanceFundingRateStream::run() {
    // Second WS dedicated to Binance mark-price/funding-rate stream
    // (different endpoint from the main MD stream). Named so perf /
    // top can separate FR traffic from the primary MD IO thread.
    bpt::common::util::set_thread_name("mdgw-binance-fr");
    const std::string fr_host = "fstream.binance.com";
    const std::string fr_port = "443";
    const std::string fr_path = "/stream?streams=!markPrice@arr@1s";

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        try {
            ioc_.restart();
            auto ws = bpt::common::ws::ws_connect(ioc_, ssl_ctx_, fr_host, fr_port, fr_path,
                                                  cfg_.so_rcvbuf_bytes, /*connect_timeout_ms=*/30000,
                                                  "bpt-md-gateway/0.1", cfg_.pinned_tls_sha256);
            bpt::common::log::info("BinanceAdapter funding-rate stream connected");

            beast::flat_buffer buf;
            while (!stop_flag_.load(std::memory_order_relaxed)) {
                ws->read(buf);
                if (!on_funding_rate_) {
                    buf.consume(buf.size());
                    continue;
                }

                uint64_t recv_ns = static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
                std::string_view sv(static_cast<const char*>(buf.data().data()), buf.data().size());
                buf.consume(buf.size());

                // Parse with simdjson on-demand — reused parser + padded buffer.
                if (padded_buf_.size() < sv.size() + simdjson::SIMDJSON_PADDING)
                    padded_buf_.resize(sv.size() + simdjson::SIMDJSON_PADDING);
                std::memcpy(padded_buf_.data(), sv.data(), sv.size());
                std::memset(padded_buf_.data() + sv.size(), 0, simdjson::SIMDJSON_PADDING);

                simdjson::ondemand::document doc;
                if (json_parser_.iterate(padded_buf_.data(), sv.size(), padded_buf_.size()).get(doc))
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
                    on_funding_rate_(fr);
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
