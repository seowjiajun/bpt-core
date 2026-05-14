add_library(backtester_lib
    app/backtester_service.cpp
    clock/clock_master.cpp
    config/loader.cpp
    data/data_loader.cpp
    exchange/binance/binance_md_server.cpp
    exchange/binance/binance_order_server.cpp
    exchange/hyperliquid/hyperliquid_info_server.cpp
    exchange/hyperliquid/hyperliquid_md_server.cpp
    exchange/hyperliquid/hyperliquid_order_server.cpp
    exchange/okx/okx_md_server.cpp
    exchange/okx/okx_order_server.cpp
    matching/matching_engine.cpp
    matching/matching_engine_book.cpp
    matching/matching_engine_fills.cpp
    messaging/aeron_bus.cpp
    messaging/backtest_ack_subscriber.cpp
    messaging/backtest_control_publisher.cpp
    results/results_collector.cpp
)

target_include_directories(backtester_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${aeron_SOURCE_DIR}/aeron-client/src/main/cpp
)

target_link_libraries(backtester_lib PUBLIC
    bpt-common
    bpt-app
    bpt::messages
    aeron_client
    fmt::fmt
    tomlplusplus::tomlplusplus
    Boost::system
    Boost::json
    prometheus-cpp::pull
    Arrow::arrow_shared
    Parquet::parquet_shared
    Threads::Threads
)

add_executable(bpt-backtester main.cpp)

target_link_libraries(bpt-backtester PRIVATE backtester_lib CLI11::CLI11)

# Smoke tool — exercises DataLoader standalone against a Parquet cache,
# no Aeron, no ticking. See bpt-backtester/tools/dl_smoke.cpp.
add_executable(dl_smoke ${CMAKE_CURRENT_SOURCE_DIR}/../tools/dl_smoke.cpp)

target_link_libraries(dl_smoke PRIVATE backtester_lib CLI11::CLI11)

# Smoke tool — drives HyperliquidInfoServer with multiple consecutive
# POST /info requests. Validates the UAF fix (req_/res_ as session
# members) for second-connection crashes.
add_executable(info_smoke ${CMAKE_CURRENT_SOURCE_DIR}/../tools/info_smoke.cpp)

target_link_libraries(info_smoke PRIVATE backtester_lib CLI11::CLI11)
