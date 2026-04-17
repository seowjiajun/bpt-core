# Refdata Library
add_library(refdata_lib
    mapping/instrument_mapping_loader.cpp
    mapping/instrument_mapping_s3_fetcher.cpp
    registry/instrument_registry.cpp
    resolver/instrument_resolver.cpp
    messaging/refdata_control_subscriber.cpp
    messaging/refdata_snapshot_publisher.cpp
    messaging/refdata_delta_publisher.cpp
    messaging/subscription_manager.cpp
    messaging/fee_schedule_publisher.cpp
    messaging/refdata_status_publisher.cpp
    config/loader.cpp
    http/rest_client.cpp
    adapter/binance/binance_parser.cpp
    adapter/binance/binance_refdata_adapter.cpp
    adapter/okx/okx_parser.cpp
    adapter/okx/okx_refdata_adapter.cpp
    adapter/hyperliquid/hyperliquid_parser.cpp
    adapter/hyperliquid/hyperliquid_refdata_adapter.cpp
    adapter/deribit/deribit_refdata_adapter.cpp
    metrics/metrics.cpp
)

target_link_libraries(refdata_lib PUBLIC
    yggdrasil
    fmt::fmt
    tomlplusplus::tomlplusplus
    nlohmann_json::nlohmann_json
    aeron_client
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    bifrost::protocol
    prometheus-cpp::pull
    aws-cpp-sdk-s3
)

target_include_directories(refdata_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${aeron_SOURCE_DIR}/aeron-client/src/main/cpp
)

# Main Executable
add_executable(refdata_app main.cpp app/refdata_app.cpp)
target_link_libraries(refdata_app PRIVATE refdata_lib)
set_target_properties(refdata_app PROPERTIES OUTPUT_NAME bpt-refdata)
