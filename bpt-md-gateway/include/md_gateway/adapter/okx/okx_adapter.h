#pragma once

#include "md_gateway/adapter/common/adapter_base.h"
#include "md_gateway/adapter/okx/okx_parser.h"

#include <atomic>
#include <bpt_common/ws/run_loop.h>

namespace bpt::md_gateway::adapter {

// OKX market-data adapter.
//
// Connects to wss://ws.okx.com:8443/ws/v5/public.
// OKX requires text-frame "ping" keepalives every 25s — Beast's built-in
// control-frame pings are disabled to prevent silent disconnects.
// Runtime subscribe/unsubscribe take effect immediately via the pending queue.
//
// Delegates read-loop mechanics (read timeout, ping thread, liveness
// watchdog) to bpt::common::ws::RunLoop — on_frame/on_tick/ping_config
// hooks express the OKX-specific bits only.
class OkxAdapter : public AdapterBase, private bpt::common::ws::RunLoop {
public:
    explicit OkxAdapter(const config::AdapterConfig& cfg, std::shared_ptr<messaging::IMdPublisher> md_pub,
                        const config::RecordingConfig& recording = {});

    [[nodiscard]] const char* exchange_name() const override { return "OKX"; }
    [[nodiscard]] bpt::common::util::LatencyHistogram& decode_latency_hist() noexcept override { return parser_.decode_lat_; }

    // Override to push the subscribe frame immediately when connected,
    // rather than waiting for on_tick (which in this Beast version may
    // not fire while OKX is actively responding to our ping thread).
    void subscribe(uint64_t instrument_id, std::string symbol, uint8_t depth = 0) override;

protected:
    std::unique_ptr<bpt::common::ws::AnyWsStream> connect_and_subscribe() override;
    void read_loop(bpt::common::ws::AnyWsStream& ws) override;
    void parse_frame(std::string_view payload, uint64_t recv_ns) override;

    // RunLoop hooks.
    void on_frame(std::string_view payload, uint64_t recv_ns) override;
    void on_tick() override;
    std::optional<bpt::common::ws::PingConfig> ping_config() const override;

private:
    OkxParser parser_;

    // RunLoop::run signature needs a 'connected' atomic; AdapterBase
    // already tracks connection state via on_connect/on_disconnect
    // callbacks, so the RunLoop flag is otherwise unused here.
    std::atomic<bool> rl_connected_{false};
};

}  // namespace bpt::md_gateway::adapter
