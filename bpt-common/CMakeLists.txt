cmake_minimum_required(VERSION 3.20)
project(bpt-common VERSION 0.1.0 LANGUAGES CXX)

# bpt-common is mostly header-only. A few translation units are compiled into a
# static library to keep heavy headers (Quill internals) out of every TU that
# only needs the corresponding bpt-common headers.

add_library(bpt-common STATIC
    src/env.cpp
    src/logging.cpp
    src/util/topology.cpp
    src/util/tsc_clock.cpp
    src/secrets/secrets_client.cpp
)

target_include_directories(bpt-common PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

find_package(fmt CONFIG REQUIRED)

target_link_libraries(bpt-common PUBLIC
    quill::quill
    fmt::fmt
)

target_compile_features(bpt-common PUBLIC cxx_std_20)
