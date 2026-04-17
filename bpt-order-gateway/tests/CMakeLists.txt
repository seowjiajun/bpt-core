include(GoogleTest)

add_executable(order_gateway_unit_tests
    unit/test_risk_checker.cpp
    unit/test_order_state_manager.cpp
    unit/test_sbe_codecs.cpp
    unit/test_adapters.cpp
    unit/test_config.cpp
    unit/test_hyperliquid_signer.cpp
    unit/test_hyperliquid_exec_parser.cpp
    unit/test_hyperliquid_action_codec.cpp
)

target_link_libraries(order_gateway_unit_tests PRIVATE
    order_gateway_lib
    GTest::gtest_main
    GTest::gmock
)

gtest_discover_tests(order_gateway_unit_tests)
