// Smoke test: bpt::common::logging::init + log calls don't throw with
// the default pattern. Catches typos in the Quill pattern string that
// would otherwise only surface when a real service tries to start.

#include <bpt_common/logging.h>
#include <filesystem>
#include <gtest/gtest.h>

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

// backend_thread_name_for: pure-string derivation feeding Quill's
// backend thread name. Must stay within Linux's 15-char TASK_COMM cap
// so multi-service hosts can distinguish quill I/O threads in top -H.
TEST(BackendThreadNameTest, StripsBptPrefix) {
    EXPECT_EQ(bpt::common::logging::backend_thread_name_for("bpt-strategy"), "strategy-log");
    EXPECT_EQ(bpt::common::logging::backend_thread_name_for("bpt-analytics"), "analytics-log");
    EXPECT_EQ(bpt::common::logging::backend_thread_name_for("bpt-refdata"), "refdata-log");
    EXPECT_EQ(bpt::common::logging::backend_thread_name_for("bpt-pricer"), "pricer-log");
}

TEST(BackendThreadNameTest, CollapsesGatewaySuffix) {
    EXPECT_EQ(bpt::common::logging::backend_thread_name_for("bpt-md-gateway"), "md-gw-log");
    EXPECT_EQ(bpt::common::logging::backend_thread_name_for("bpt-order-gateway"), "order-gw-log");
    EXPECT_EQ(bpt::common::logging::backend_thread_name_for("order-gateway"), "order-gw-log");
}

TEST(BackendThreadNameTest, CollapsesGatewayMidString) {
    // Instance-qualified names where venue is appended after "gateway"
    // must also collapse, otherwise they overflow the 15-char cap.
    EXPECT_EQ(bpt::common::logging::backend_thread_name_for("bpt-md-gateway-okx"), "md-gw-okx-log");
    EXPECT_EQ(bpt::common::logging::backend_thread_name_for("bpt-md-gateway-hl"), "md-gw-hl-log");
}

TEST(BackendThreadNameTest, PassesThroughAlreadyAbbreviated) {
    // Services that already use the short "-gw" form in service_name
    // (recommended shape) are a no-op in the collapse step.
    EXPECT_EQ(bpt::common::logging::backend_thread_name_for("bpt-md-gw-okx"), "md-gw-okx-log");
    EXPECT_EQ(bpt::common::logging::backend_thread_name_for("bpt-md-gw-okx-0"), "md-gw-okx-0-log");
}

TEST(BackendThreadNameTest, TruncatesAt15Chars) {
    auto name = bpt::common::logging::backend_thread_name_for("bpt-md-gw-binance-0");
    EXPECT_LE(name.size(), 15u);
    // Prefix is preserved; only the tail is lost to truncation.
    EXPECT_EQ(name.substr(0, 11), "md-gw-binan");
}

TEST(BackendThreadNameTest, HandlesNamesWithoutBptPrefix) {
    EXPECT_EQ(bpt::common::logging::backend_thread_name_for("bridge"), "bridge-log");
}
