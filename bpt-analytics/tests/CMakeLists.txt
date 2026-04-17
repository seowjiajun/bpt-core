include(GoogleTest)

add_executable(analytics_unit_tests
    unit/test_fill_rate_tracker.cpp
    unit/test_markout_tracker.cpp
    unit/test_toxicity_scorer.cpp
)

target_link_libraries(analytics_unit_tests PRIVATE
    analytics_lib
    GTest::gtest_main
    GTest::gmock
)

gtest_discover_tests(analytics_unit_tests)
