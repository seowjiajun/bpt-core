include(GoogleTest)

add_executable(heimdall_unit_tests
    unit/test_risk_checker.cpp
    unit/test_order_state_manager.cpp
    unit/test_sbe_codecs.cpp
    unit/test_adapters.cpp
    unit/test_config.cpp
    unit/test_hyperliquid_signer.cpp
)

target_link_libraries(heimdall_unit_tests PRIVATE
    heimdall_lib
    GTest::gtest_main
    GTest::gmock
)

gtest_discover_tests(heimdall_unit_tests)
