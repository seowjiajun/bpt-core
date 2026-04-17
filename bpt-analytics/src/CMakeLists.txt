add_library(analytics_lib
    analysis/fill_rate_tracker.cpp
    analysis/markout_tracker.cpp
    analysis/toxicity_scorer.cpp
    config/config.cpp
    app/tyr_app.cpp
)

target_include_directories(analytics_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

target_link_libraries(analytics_lib PUBLIC
    bifrost::protocol
    yggdrasil
    aeron_client
    fmt::fmt
    tomlplusplus::tomlplusplus
    Threads::Threads
)

target_compile_features(analytics_lib PUBLIC cxx_std_23)

add_executable(tyr main.cpp)
target_link_libraries(tyr PRIVATE analytics_lib)
set_target_properties(tyr PROPERTIES OUTPUT_NAME bpt-analytics)
