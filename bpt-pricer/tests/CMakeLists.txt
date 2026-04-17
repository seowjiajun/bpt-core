include(GoogleTest)

add_executable(pricer_tests
    test_black_scholes.cpp
    test_iv_solver.cpp
    test_forward_curve.cpp
)

target_link_libraries(pricer_tests PRIVATE
    pricer_lib
    GTest::gtest_main
)

gtest_discover_tests(pricer_tests)
