add_library(heimdall_lib
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
    adapter/common/order_adapter_base.cpp
    adapter/binance/binance_order_adapter.cpp
    adapter/binance/binance_exec_parser.cpp
    adapter/okx/okx_order_adapter.cpp
    adapter/okx/okx_exec_parser.cpp
    adapter/deribit/deribit_order_adapter.cpp
    adapter/deribit/deribit_exec_parser.cpp
    adapter/hyperliquid/hyperliquid_order_adapter.cpp
    adapter/hyperliquid/hyperliquid_exec_parser.cpp
    adapter/hyperliquid/hyperliquid_signer.cpp
    metrics/metrics.cpp
)

target_link_libraries(heimdall_lib PUBLIC
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

target_include_directories(heimdall_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${aeron_SOURCE_DIR}/aeron-client/src/main/cpp
)

add_executable(heimdall_app main.cpp app/heimdall_app.cpp)
target_link_libraries(heimdall_app PRIVATE heimdall_lib)
set_target_properties(heimdall_app PROPERTIES OUTPUT_NAME heimdall)
