#pragma once

#include "md_gateway/adapter/common/adapter_base.h"
#include "md_gateway/adapter/okx/okx_parser.h"

namespace bpt::md_gateway::adapter {

// OKX market-data adapter.
//
// Connects to wss://ws.okx.com:8443/ws/v5/public.
// OKX requires text-frame "ping" keepalives every 25s — Beast's built-in
// control-frame pings are disabled to prevent silent disconnects.
// Runtime subscribe/unsubscribe take effect immediately via the pending queue.
class OkxAdapter : public AdapterBase {
public:
    explicit OkxAdapter(const config::AdapterConfig& cfg, std::shared_ptr<messaging::IMdPublisher> md_pub);

    [[nodiscard]] const char* exchange_name() const override { return "OKX"; }
    [[nodiscard]] bpt::common::util::LatencyHistogram& decode_latency_hist() noexcept override { return parser_.decode_lat_; }

protected:
    std::unique_ptr<bpt::common::ws::AnyWsStream> connect_and_subscribe() override;
    void read_loop(bpt::common::ws::AnyWsStream& ws) override;
    void parse_frame(std::string_view payload, uint64_t recv_ns) override;

private:
    void send_instrument_subs(bpt::common::ws::AnyWsStream& ws, const std::string& symbol, uint8_t depth);

    OkxParser parser_;
};

}  // namespace bpt::md_gateway::adapter
