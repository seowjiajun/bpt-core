include(GoogleTest)

add_executable(refdata_tests
    main_test.cpp
    test_config.cpp
    test_instrument_mapping.cpp
    test_sbe_codecs.cpp
    test_binance_parser.cpp
    test_okx_parser.cpp
    test_hyperliquid_parser.cpp
    test_instrument_registry.cpp
)

target_link_libraries(refdata_tests PRIVATE
    refdata_lib
    GTest::gtest_main
    GTest::gmock
)

gtest_discover_tests(refdata_tests)
