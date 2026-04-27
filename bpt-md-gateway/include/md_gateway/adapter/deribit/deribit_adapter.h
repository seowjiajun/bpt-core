#pragma once

#include "md_gateway/adapter/common/adapter_base.h"
#include "md_gateway/adapter/deribit/deribit_parser.h"

#include <atomic>
#include <cstdint>
#include <bpt_common/ws/run_loop.h>

namespace bpt::md_gateway::adapter {

// Deribit market-data adapter.
//
// Connects to wss://www.deribit.com/ws/api/v2 and uses JSON-RPC 2.0.
// Sends public/set_heartbeat after connect; responds to test_request with
// public/test — Deribit disconnects within 30s if this is missed.
// Detects order book gaps via prev_change_id; affected instruments are
// automatically resubscribed.
//
// Heartbeat: Deribit's set_heartbeat keeps the session alive, so
// ping_config is left at nullopt (no application ping thread).
class DeribitAdapter : public AdapterBase, private bpt::common::ws::RunLoop {
public:
    explicit DeribitAdapter(const config::AdapterConfig& cfg, std::shared_ptr<messaging::IMdPublisher> md_pub,
                        const config::RecordingConfig& recording = {});

    // Also clears gap-detection state for the instrument in the parser.
    void unsubscribe(uint64_t instrument_id) override;

    // Push subscribe frames immediately when connected — on_tick can't
    // be relied upon because RunLoop's sync ws.read() doesn't honour
    // expires_after in this Beast version (see OkxAdapter commit).
    void subscribe(uint64_t instrument_id, std::string symbol, uint8_t depth = 0) override;

    [[nodiscard]] const char* exchange_name() const override { return "DERIBIT"; }
    [[nodiscard]] bpt::common::util::LatencyHistogram& decode_latency_hist() noexcept override { return parser_.decode_lat_; }

protected:
    // 2 s back-off — Deribit is slower to recover than CEXs.
    std::chrono::milliseconds reconnect_delay() const override;

    std::unique_ptr<bpt::common::ws::AnyWsStream> connect_and_subscribe() override;
    void read_loop(bpt::common::ws::AnyWsStream& ws) override;
    void parse_frame(std::string_view payload, uint64_t recv_ns) override;

    void on_frame(std::string_view payload, uint64_t recv_ns) override;
    void on_tick() override;

private:
    DeribitParser parser_;
    std::atomic<uint64_t> rpc_id_{1};
    std::atomic<bool> rl_connected_{false};

    // Set by the publisher thread when the parser detects a Deribit test_request
    // heartbeat.  The IO thread (via RunLoop's on_tick) reads this flag and
    // sends the WS response — writes must happen on the stream's owner thread.
    std::atomic<bool> needs_test_response_{false};
};

}  // namespace bpt::md_gateway::adapter
