cmake_minimum_required(VERSION 3.20)
project(bpt-messages VERSION 1.0.0 LANGUAGES CXX)

# Header-only interface library — no compilation required.
# Consumers add:
#   find_package(bpt-messages REQUIRED)
#   target_link_libraries(my_target PRIVATE bpt::messages)
add_library(bpt_messages INTERFACE)
add_library(bpt::messages ALIAS bpt_messages)

target_include_directories(bpt_messages INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/generated/cpp>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(bpt_messages INTERFACE cxx_std_17)

# ── Compile and round-trip check (CI) ───────────────────────────────────────
# Build and run the compile_check executable to verify all generated headers
# compile cleanly and that encode→decode round-trips produce identical values.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_executable(bpt_messages_check ci/compile_check.cpp)
    target_link_libraries(bpt_messages_check PRIVATE bpt::messages)
    target_compile_features(bpt_messages_check PRIVATE cxx_std_17)
    enable_testing()
    add_test(NAME bpt_messages_check COMMAND bpt_messages_check)
endif()

# ── Install rules (optional, for system-wide install) ───────────────────────
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS bpt_messages EXPORT bpt-messages-targets)
install(DIRECTORY generated/cpp/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT bpt-messages-targets
    NAMESPACE bpt::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/bpt-messages
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/bpt-messages-config-version.cmake"
    COMPATIBILITY SameMajorVersion
)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/bpt-messages-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/bpt-messages-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/bpt-messages
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/bpt-messages-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/bpt-messages-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/bpt-messages
)
