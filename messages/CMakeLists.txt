cmake_minimum_required(VERSION 3.20)
project(bifrost-protocol VERSION 1.0.0 LANGUAGES CXX)

# Header-only interface library — no compilation required.
# Consumers add:
#   find_package(bifrost-protocol REQUIRED)
#   target_link_libraries(my_target PRIVATE bifrost::protocol)
add_library(bifrost_protocol INTERFACE)
add_library(bifrost::protocol ALIAS bifrost_protocol)
# New canonical name — consumers should migrate to bpt::messages
add_library(bpt::messages ALIAS bifrost_protocol)

target_include_directories(bifrost_protocol INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/generated/cpp>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(bifrost_protocol INTERFACE cxx_std_17)

# ── Compile and round-trip check (CI) ───────────────────────────────────────
# Build and run the compile_check executable to verify all generated headers
# compile cleanly and that encode→decode round-trips produce identical values.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_executable(bifrost_protocol_check ci/compile_check.cpp)
    target_link_libraries(bifrost_protocol_check PRIVATE bifrost::protocol)
    target_compile_features(bifrost_protocol_check PRIVATE cxx_std_17)
    enable_testing()
    add_test(NAME bifrost_protocol_check COMMAND bifrost_protocol_check)
endif()

# ── Install rules (optional, for system-wide install) ───────────────────────
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS bifrost_protocol EXPORT bifrost-protocol-targets)
install(DIRECTORY generated/cpp/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT bifrost-protocol-targets
    NAMESPACE bifrost::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/bifrost-protocol
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/bifrost-protocol-config-version.cmake"
    COMPATIBILITY SameMajorVersion
)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/bifrost-protocol-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/bifrost-protocol-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/bifrost-protocol
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/bifrost-protocol-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/bifrost-protocol-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/bifrost-protocol
)
