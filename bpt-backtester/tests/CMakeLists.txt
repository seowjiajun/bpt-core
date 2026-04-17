include(GoogleTest)

add_executable(backtester_unit_tests
    unit/test_config.cpp
    unit/test_data_loader.cpp
    unit/test_matching_engine.cpp
    unit/test_results_collector.cpp
)

target_link_libraries(backtester_unit_tests PRIVATE
    backtester_lib
    GTest::gtest_main
    GTest::gmock
    Arrow::arrow_shared
    Parquet::parquet_shared
)

gtest_discover_tests(backtester_unit_tests DISCOVERY_MODE PRE_TEST)
