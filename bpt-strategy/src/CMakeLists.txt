add_library(strategy_lib
    app/strategy_app.cpp
    app/startup_gate.cpp
    backtest/backtest_client.cpp
    config/config.cpp
    dashboard/portfolio_snapshot_publisher.cpp
    metrics/metrics.cpp
    refdata/refdata_client.cpp
    refdata/instrument_cache.cpp
    refdata/fee_cache.cpp
    refdata/funding_rate_cache.cpp
    strategy/canonical_resolver.cpp
    strategy/hmm_filter.cpp
    strategy/hmm_strategy.cpp
    strategy/momentum_strategy.cpp
    strategy/ofi_strategy.cpp
    strategy/position_tracker.cpp
    strategy/avellaneda_stoikov_strategy.cpp
    strategy/funding_arb_strategy.cpp
    strategy/hurst_estimator.cpp
    strategy/ofi_calculator.cpp
    strategy/regime_detector.cpp
    strategy/regime_switch_strategy.cpp
    strategy/vwap_reversion_strategy.cpp
    strategy/realized_vol_estimator.cpp
    strategy/short_vol_strategy.cpp
    strategy/strategy_factory.cpp
    md/md_client.cpp
    vol/vol_surface_client.cpp
    order/order_gateway_client.cpp
    order/order_manager.cpp
)

target_link_libraries(strategy_lib PUBLIC
    yggdrasil
    aeron_client
    bifrost::protocol
    analytics_lib
    fmt::fmt
    tomlplusplus::tomlplusplus
    prometheus-cpp::pull
    Threads::Threads
)

target_include_directories(strategy_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${aeron_SOURCE_DIR}/aeron-client/src/main/cpp
)

add_executable(fenrir main.cpp)
target_link_libraries(fenrir PRIVATE strategy_lib)

include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported)
if(ipo_supported)
    set_property(TARGET fenrir PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()
