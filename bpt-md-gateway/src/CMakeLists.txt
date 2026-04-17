# Huginn library (everything except main)
add_library(md_gateway_lib
    config/loader.cpp
    md/md_encoder.cpp
    md/md_validator.cpp
    messaging/md_publisher.cpp
    messaging/ack_publisher.cpp
    messaging/funding_rate_publisher.cpp
    adapter/common/subscription_map.cpp
    adapter/common/adapter_base.cpp
    adapter/binance/binance_parser.cpp
    adapter/binance/binance_adapter.cpp
    adapter/okx/okx_parser.cpp
    adapter/okx/okx_adapter.cpp
    adapter/deribit/deribit_parser.cpp
    adapter/deribit/deribit_adapter.cpp
    adapter/hyperliquid/hyperliquid_parser.cpp
    adapter/hyperliquid/hyperliquid_adapter.cpp
    subscription/subscription_manager.cpp
    metrics/metrics.cpp
)

target_link_libraries(md_gateway_lib PUBLIC
    yggdrasil
    fmt::fmt
    FastFloat::fast_float
    tomlplusplus::tomlplusplus
    Boost::system
    simdjson::simdjson
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    aeron_client
    bifrost::protocol
    prometheus-cpp::pull
)

target_include_directories(md_gateway_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${aeron_SOURCE_DIR}/aeron-client/src/main/cpp
)

# Main executable
add_executable(md_gateway_app main.cpp app/md_gateway_app.cpp)
target_link_libraries(md_gateway_app PRIVATE md_gateway_lib)
set_target_properties(md_gateway_app PROPERTIES OUTPUT_NAME bpt-md-gateway)
