include(GoogleTest)

add_executable(md_gateway_unit_tests
    unit/test_sbe_codecs.cpp
    unit/test_adapters.cpp
    unit/test_md_validator.cpp
)

target_link_libraries(md_gateway_unit_tests PRIVATE
    md_gateway_lib
    GTest::gtest_main
    GTest::gmock
)

gtest_discover_tests(md_gateway_unit_tests)

add_executable(md_gateway_component_tests
    component/test_okx_adapter.cpp
    component/test_binance_adapter.cpp
    component/test_hyperliquid_adapter.cpp
    component/test_deribit_adapter.cpp
    component/test_subscription_manager.cpp
    component/test_okx_l2_book.cpp
    component/test_hyperliquid_funding.cpp
)

target_link_libraries(md_gateway_component_tests PRIVATE
    md_gateway_lib
    GTest::gtest_main
    GTest::gmock
)

target_include_directories(md_gateway_component_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/component
)

gtest_discover_tests(md_gateway_component_tests)
