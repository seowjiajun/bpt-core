// Unit tests for bpt::app::load_base_settings.
//
// The run() template itself isn't unit-tested here — it requires a live
// Aeron MediaDriver and TSC calibration, which is awkward in a pure unit
// test. That path gets exercised implicitly when services start using it.

#include "bpt_app/base_settings.h"

#include <gtest/gtest.h>
#include <stdexcept>
#include <toml++/toml.hpp>

namespace {

using bpt::app::BaseSettings;
using bpt::app::Env;
using bpt::app::env_from_string;
using bpt::app::load_base_settings;
using bpt::app::to_string;

toml::table parse(const char* src) {
    return toml::parse(src);
}

// ── Env enum + string helpers ────────────────────────────────────────────────

TEST(EnvTest, ToStringRoundTrip) {
    EXPECT_EQ(to_string(Env::DEV), "dev");
    EXPECT_EQ(to_string(Env::QA), "qa");
    EXPECT_EQ(to_string(Env::PROD), "prod");
}

TEST(EnvTest, FromStringKnownValues) {
    EXPECT_EQ(env_from_string("dev"), Env::DEV);
    EXPECT_EQ(env_from_string("qa"), Env::QA);
    EXPECT_EQ(env_from_string("prod"), Env::PROD);
}

TEST(EnvTest, FromStringRejectsTypo) {
    // Catches the "prd"/"prood"/"" class of silent-skip-prod-checks bugs.
    EXPECT_THROW(env_from_string("prd"), std::runtime_error);
    EXPECT_THROW(env_from_string(""), std::runtime_error);
    EXPECT_THROW(env_from_string("PROD"), std::runtime_error) << "case-sensitive on purpose";
}

// ── load_base_settings ───────────────────────────────────────────────────────

TEST(BaseSettingsLoader, MissingEnvironmentThrows) {
    BaseSettings base;
    auto root = parse("");
    EXPECT_THROW(load_base_settings(root, base), std::runtime_error)
        << "environment is required — empty TOML must throw so typos can't silently skip it";
}

TEST(BaseSettingsLoader, ReadsEnvironment) {
    BaseSettings base;
    auto root = parse(R"(environment = "prod")");
    load_base_settings(root, base);
    EXPECT_EQ(base.environment, Env::PROD);
    EXPECT_TRUE(base.is_prod());
    EXPECT_FALSE(base.is_qa());
    EXPECT_FALSE(base.is_dev());
}

TEST(BaseSettingsLoader, ReadsAeronMediaDriverDir) {
    BaseSettings base;
    auto root = parse(R"(
        environment = "dev"
        [aeron]
        media_driver_dir = "/dev/shm/aeron-bpt"
    )");
    load_base_settings(root, base);
    EXPECT_EQ(base.media_driver_dir, "/dev/shm/aeron-bpt");
}

TEST(BaseSettingsLoader, ReadsMetricsPort) {
    BaseSettings base;
    auto root = parse(R"(
        environment = "dev"
        [metrics]
        port = 9103
    )");
    load_base_settings(root, base);
    EXPECT_EQ(base.metrics_port, 9103);
}

TEST(BaseSettingsLoader, ReadsLoggingBlock) {
    // Full delegation to bpt::common::logging::from_toml — this test just
    // verifies the [logging] table reaches it (at least one field takes
    // effect). Exhaustive logging field parsing lives in bpt-common's
    // own tests.
    BaseSettings base;
    auto root = parse(R"(
        environment = "dev"
        [logging]
        level = "debug"
        dir   = "/var/log/myservice"
    )");
    load_base_settings(root, base);
    EXPECT_EQ(base.logging.level, "debug");
    EXPECT_EQ(base.logging.log_dir, "/var/log/myservice");
}

TEST(BaseSettingsLoader, AllTogether) {
    BaseSettings base;
    auto root = parse(R"(
        environment = "qa"
        [aeron]
        media_driver_dir = "/tmp/aeron"
        [logging]
        level = "info"
        [metrics]
        port = 9999
    )");
    load_base_settings(root, base);
    EXPECT_EQ(base.environment, Env::QA);
    EXPECT_EQ(base.media_driver_dir, "/tmp/aeron");
    EXPECT_EQ(base.logging.level, "info");
    EXPECT_EQ(base.metrics_port, 9999);
}

TEST(BaseSettingsLoader, PreservesCalibrateTscWhenAbsent) {
    // calibrate_tsc is preset by the caller (backtester sets false); the
    // loader must not clobber it since there's no TOML hook.
    BaseSettings base;
    base.calibrate_tsc = false;
    auto root = parse(R"(environment = "dev")");
    load_base_settings(root, base);
    EXPECT_FALSE(base.calibrate_tsc);
}

}  // namespace
