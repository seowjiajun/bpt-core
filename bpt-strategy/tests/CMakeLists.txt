add_executable(strategy_unit_tests
    unit/test_instrument_cache.cpp
    unit/test_volatility_gate.cpp
    unit/test_ofi_calculator.cpp
    unit/test_position_tracker.cpp
    unit/test_regime_detector.cpp
)

target_link_libraries(strategy_unit_tests PRIVATE
    strategy_lib
    GTest::gtest_main
    GTest::gmock
)

include(GoogleTest)
gtest_discover_tests(strategy_unit_tests)
