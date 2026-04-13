cmake_minimum_required(VERSION 3.20)
project(bpt-core VERSION 0.1.0 LANGUAGES CXX)

# C++23 globally — all services use at least C++20, heimdall/jormungandr use C++23.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Release / low-latency flags ───────────────────────────────────────────────
if(CMAKE_BUILD_TYPE MATCHES "Release|RelWithDebInfo")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported)
    if(ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
    add_compile_options(-O3 -march=native -flto -DNDEBUG)
endif()

# ── Aeron (fetched once, shared by all services) ──────────────────────────────
include(FetchContent)
FetchContent_Declare(
    aeron
    GIT_REPOSITORY https://github.com/real-logic/aeron.git
    GIT_TAG        1.44.1
)
FetchContent_MakeAvailable(aeron)

# fast_float — header-only, SWAR-accelerated string-to-float (used by huginn)
FetchContent_Declare(
    fast_float
    GIT_REPOSITORY https://github.com/fastfloat/fast_float.git
    GIT_TAG        v6.1.6
)
FetchContent_MakeAvailable(fast_float)

# Quill — low-latency async logger (formats on backend thread, ~3-10ns hot path)
# QUILL_FMT_EXTERNAL: reuse the vcpkg fmt already in the build, avoiding ODR issues.
set(QUILL_FMT_EXTERNAL ON  CACHE BOOL "" FORCE)
set(QUILL_NO_EXCEPTIONS  OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    quill
    GIT_REPOSITORY https://github.com/odygrd/quill.git
    GIT_TAG        v8.1.0
)
FetchContent_MakeAvailable(quill)

# ── vcpkg dependencies (resolved once, available to all subdirectories) ───────
find_package(fmt            REQUIRED)
find_package(tomlplusplus   CONFIG REQUIRED)
find_package(OpenSSL        REQUIRED)
find_package(libsecp256k1   CONFIG REQUIRED)
find_package(Boost          REQUIRED COMPONENTS system json)
find_package(simdjson       CONFIG REQUIRED)
find_package(prometheus-cpp CONFIG REQUIRED)
find_package(Threads        REQUIRED)
find_package(nlohmann_json  REQUIRED)
find_package(GTest          REQUIRED)
find_package(aws-cpp-sdk-core           CONFIG REQUIRED)
find_package(aws-cpp-sdk-s3             CONFIG REQUIRED)
find_package(aws-cpp-sdk-secretsmanager CONFIG REQUIRED)
find_package(Arrow   REQUIRED)
find_package(Parquet REQUIRED)

# ── Internal libraries ────────────────────────────────────────────────────────
add_subdirectory(third_party/yggdrasil)
add_subdirectory(bifrost/protocol)

# ── Services ──────────────────────────────────────────────────────────────────
enable_testing()

add_subdirectory(fenrir)
add_subdirectory(huginn)
add_subdirectory(heimdall)
add_subdirectory(muninn)
add_subdirectory(surtr)
add_subdirectory(jormungandr)
add_subdirectory(dashboard/bridge)
