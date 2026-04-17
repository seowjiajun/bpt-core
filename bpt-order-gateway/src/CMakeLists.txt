add_library(order_gateway_lib
    config/loader.cpp
    messaging/account_snapshot_publisher.cpp
    messaging/exec_report_publisher.cpp
    messaging/heartbeat_publisher.cpp
    messaging/order_subscriber.cpp
    refdata/instrument_cache.cpp
    refdata/refdata_client.cpp
    risk/risk_checker.cpp
    order/order_state_manager.cpp
    order/order_processor.cpp
    adapter/common/beast_https_client.cpp
    adapter/common/order_adapter_base.cpp
    adapter/binance/binance_action_codec.cpp
    adapter/binance/binance_auth.cpp
    adapter/binance/binance_https_client.cpp
    adapter/binance/binance_user_data_ws.cpp
    adapter/binance/binance_order_adapter.cpp
    adapter/binance/binance_exec_parser.cpp
    adapter/okx/okx_action_codec.cpp
    adapter/okx/okx_auth.cpp
    adapter/okx/okx_https_client.cpp
    adapter/okx/okx_instruments_service.cpp
    adapter/okx/okx_ws_client.cpp
    adapter/okx/okx_order_adapter.cpp
    adapter/okx/okx_exec_parser.cpp
    adapter/deribit/deribit_action_codec.cpp
    adapter/deribit/deribit_ws_client.cpp
    adapter/deribit/deribit_order_adapter.cpp
    adapter/deribit/deribit_exec_parser.cpp
    adapter/hyperliquid/hyperliquid_action_codec.cpp
    adapter/hyperliquid/hyperliquid_exec_emitter.cpp
    adapter/hyperliquid/hyperliquid_https_client.cpp
    adapter/hyperliquid/hyperliquid_ws_client.cpp
    adapter/hyperliquid/hyperliquid_order_adapter.cpp
    adapter/hyperliquid/hyperliquid_exec_parser.cpp
    adapter/hyperliquid/hyperliquid_signer.cpp
    metrics/metrics.cpp
)

target_link_libraries(order_gateway_lib PUBLIC
    yggdrasil
    aeron_client
    bifrost::protocol
    fmt::fmt
    tomlplusplus::tomlplusplus
    Boost::system
    Boost::json
    OpenSSL::SSL
    OpenSSL::Crypto
    libsecp256k1::secp256k1
    Threads::Threads
    prometheus-cpp::pull
)

target_include_directories(order_gateway_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${aeron_SOURCE_DIR}/aeron-client/src/main/cpp
)

add_executable(gateway_app main.cpp app/gateway_app.cpp)
target_link_libraries(gateway_app PRIVATE order_gateway_lib)
set_target_properties(gateway_app PROPERTIES OUTPUT_NAME bpt-order-gateway)
