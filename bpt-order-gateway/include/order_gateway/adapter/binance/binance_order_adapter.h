#pragma once

#include "order_gateway/adapter/binance/binance_exec_decoder.h"
#include "order_gateway/adapter/binance/binance_https_client.h"
#include "order_gateway/adapter/binance/binance_user_data_ws.h"
#include "order_gateway/adapter/common/credentials.h"
#include "order_gateway/adapter/common/order_adapter_base.h"

#include <string>

namespace bpt::order_gateway::adapter {

// BinanceOrderAdapter places orders via REST (POST /api/v3/order) and
// subscribes to the user-data WebSocket (via listenKey) for exec
// reports. Transport is split across four components:
//
//   - binance_https_client  — TLS REST client
//   - binance_auth          — query-string HMAC-SHA256 signer
//   - binance_action_encoder — pure query-param builders
//   - binance_user_data_ws  — listenKey lifecycle + read loop
//
// The adapter itself is pure orchestration: ctor wiring, send_*
// methods that compose codec + auth + https_client, and a user-data
// dispatch callback into BinanceExecDecoder.
//
// Credentials are passed directly via ExchangeCredentials (api_key, secret_key).
class BinanceOrderAdapter : public OrderAdapterBase {
public:
    BinanceOrderAdapter(const config::AdapterConfig& cfg, const ExchangeCredentials& creds);

    void send_new_order(const bpt::messages::NewOrder& order) override;
    void send_cancel(const bpt::messages::CancelOrder& cancel, const std::string& native_symbol) override;
    void send_cancel_all(uint64_t instrument_id) override;
    void send_modify(const bpt::messages::ModifyOrder& modify, const std::string& native_symbol) override;

    [[nodiscard]] bpt::messages::ExchangeId::Value exchange_id() const override {
        return bpt::messages::ExchangeId::BINANCE;
    }
    [[nodiscard]] const char* exchange_name() const override { return "BINANCE"; }
    [[nodiscard]] bool is_connected() const override { return connected_.load(std::memory_order_relaxed); }

    AccountSnapshotData fetch_account_snapshot(uint64_t correlation_id) override;

protected:
    void connect_and_run() override;

private:
    void handle_user_data_message(const std::string& payload, uint64_t recv_ns);

    std::string api_key_;
    std::string secret_key_;

    BinanceExecDecoder decoder_;
    binance::BinanceHttpsClient https_client_;
    binance::BinanceUserDataWs user_data_ws_;
};

}  // namespace bpt::order_gateway::adapter
