project(bridge VERSION 0.1.0)

# WebSocket bridge: subscribes to Aeron streams, forwards as JSON over WebSocket.
# Uses Boost.Beast (header-only, already in the project dep tree via Boost).

add_library(bridge_lib
    src/message_encoder.cpp
    src/position_tracker.cpp
    src/ws_server.cpp
    src/account_subscriber.cpp
    src/exec_subscriber.cpp
    src/md_subscriber.cpp
    src/config_loader.cpp
)

target_include_directories(bridge_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${aeron_SOURCE_DIR}/aeron-client/src/main/cpp
)

target_link_libraries(bridge_lib PUBLIC
    yggdrasil
    bpt::messages
    analytics_lib
    aeron_client
    fmt::fmt
    nlohmann_json::nlohmann_json
    tomlplusplus::tomlplusplus
    Boost::system
    Threads::Threads
)

add_executable(bridge_app src/main.cpp)
target_link_libraries(bridge_app PRIVATE bridge_lib)
set_target_properties(bridge_app PROPERTIES OUTPUT_NAME bridge)
