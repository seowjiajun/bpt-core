cmake_minimum_required(VERSION 3.20)
project(bpt-core VERSION 0.1.0 LANGUAGES CXX)

# Bazel is the primary build system for this repo. CMake survives only for
# bpt-backtester because its Arrow/Parquet dependency has no clean Bazel
# story yet. All other services (bpt-strategy, bpt-md-gateway,
# bpt-order-gateway, bpt-refdata, bpt-pricer, bpt-analytics, dashboard/bridge)
# build exclusively through Bazel.
#
# When backtester's Parquet consumer is replaced with a simpler binary format
# (or rules_foreign_cc Arrow lands), this file and vcpkg.json can be deleted
# and the `cpp-backtester-cmake` CI job removed.

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_BUILD_TYPE MATCHES "Release|RelWithDebInfo")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported)
    if(ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
    add_compile_options(-O3 -march=native -flto -DNDEBUG)
endif()

# ── FetchContent deps used only by backtester (+ yggdrasil transitively) ────
include(FetchContent)

FetchContent_Declare(
    aeron
    GIT_REPOSITORY https://github.com/real-logic/aeron.git
    GIT_TAG        1.44.1
)
FetchContent_MakeAvailable(aeron)

# QUILL_FMT_EXTERNAL: share the vcpkg fmt already in the build to avoid ODR.
set(QUILL_FMT_EXTERNAL ON  CACHE BOOL "" FORCE)
set(QUILL_NO_EXCEPTIONS  OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    quill
    GIT_REPOSITORY https://github.com/odygrd/quill.git
    GIT_TAG        v8.1.0
)
FetchContent_MakeAvailable(quill)

# ── vcpkg deps (only those backtester actually consumes) ─────────────────────
find_package(fmt            REQUIRED)
find_package(tomlplusplus   CONFIG REQUIRED)
find_package(Boost          REQUIRED COMPONENTS system json)
find_package(prometheus-cpp CONFIG REQUIRED)
find_package(Threads        REQUIRED)
find_package(GTest          REQUIRED)
find_package(Arrow   REQUIRED)
find_package(Parquet REQUIRED)
find_package(CLI11   CONFIG REQUIRED)

# ── Libraries + the one remaining service ────────────────────────────────────
add_subdirectory(bpt-common)
add_subdirectory(bpt-app)
add_subdirectory(messages)

enable_testing()
add_subdirectory(bpt-backtester)
