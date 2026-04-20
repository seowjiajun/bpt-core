// Smoke test: bpt::common::logging::init + log calls don't throw with
// the default pattern. Catches typos in the Quill pattern string that
// would otherwise only surface when a real service tries to start.

#include <bpt_common/logging.h>

#include <gtest/gtest.h>
#include <filesystem>

TEST(LoggingSmokeTest, InitWithDefaultPatternDoesNotThrow) {
    bpt::common::logging::LogConfig cfg;
    cfg.log_dir = (std::filesystem::temp_directory_path() / "bpt-logging-test").string();
    cfg.file = false;
    cfg.console = false;  // don't spam the test runner

    // The real check: init() must not throw while parsing our default
    // pattern. If Quill rejects any of %(time), %(log_level_short_code),
    // or %(logger), this fails.
    ASSERT_NO_THROW(bpt::common::logging::init("test-service", cfg));
    ASSERT_NE(bpt::common::logging::get_default_logger(), nullptr);

    // Named sub-logger accessor — same pattern, different name.
    auto* module_log = bpt::common::logging::get_logger("TestModule");
    ASSERT_NE(module_log, nullptr);

    // Both call paths through the log:: free functions and the
    // per-logger wrappers must compile and run without throwing.
    ASSERT_NO_THROW(bpt::common::log::info("smoke default logger: {}", 42));
    ASSERT_NO_THROW(bpt::common::log::info(module_log, "smoke named logger: {}", "ok"));
    ASSERT_NO_THROW(bpt::common::log::warn(module_log, "warn check"));
    ASSERT_NO_THROW(bpt::common::log::error(module_log, "error check"));
}
