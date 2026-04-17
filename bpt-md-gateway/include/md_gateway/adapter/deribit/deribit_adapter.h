#pragma once

#include "md_gateway/adapter/common/adapter_base.h"
#include "md_gateway/adapter/deribit/deribit_parser.h"

#include <atomic>
#include <cstdint>

namespace bpt::md_gateway::adapter {

// Deribit market-data adapter.
//
// Connects to wss://www.deribit.com/ws/api/v2 and uses JSON-RPC 2.0.
// Sends public/set_heartbeat after connect; responds to test_request with
// public/test — Deribit disconnects within 30s if this is missed.
// Detects order book gaps via prev_change_id; affected instruments are
// automatically resubscribed.
class DeribitAdapter : public AdapterBase {
public:
    explicit DeribitAdapter(const config::AdapterConfig& cfg, std::shared_ptr<messaging::IMdPublisher> md_pub);

    // Also clears gap-detection state for the instrument in the parser.
    void unsubscribe(uint64_t instrument_id) override;

    [[nodiscard]] const char* exchange_name() const override { return "DERIBIT"; }
    [[nodiscard]] ygg::util::LatencyHistogram& decode_latency_hist() noexcept override { return parser_.decode_lat_; }

protected:
    // 2 s back-off — Deribit is slower to recover than CEXs.
    std::chrono::milliseconds reconnect_delay() const override;

    std::unique_ptr<ygg::ws::AnyWsStream> connect_and_subscribe() override;
    void read_loop(ygg::ws::AnyWsStream& ws) override;
    void parse_frame(std::string_view payload, uint64_t recv_ns) override;

private:
    void send_subscribe_rpc(ygg::ws::AnyWsStream& ws, const std::string& symbol, uint8_t depth);
    void send_test_response(ygg::ws::AnyWsStream& ws);

    DeribitParser parser_;
    std::atomic<uint64_t> rpc_id_{1};

    // Set by the publisher thread when the parser detects a Deribit test_request
    // heartbeat.  The IO thread reads this flag each iteration and sends the WS
    // response (which must happen on the thread that owns the ygg::ws::WsStream).
    std::atomic<bool> needs_test_response_{false};
};

}  // namespace bpt::md_gateway::adapter
