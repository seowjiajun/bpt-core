/// Smoke test: read back a real canon file produced by bpt-canon-replay.
///
/// Disabled unless the file exists — keeps CI passing on hosts that don't
/// have the test tape. Run locally after:
///   bazel-bin/bpt-canon/bpt-canon-replay --wslog ... --output /tmp/...
///
/// Validates: file header parses; record count is non-zero; ts_ns is
/// monotonic-or-equal within the file.

#include <canon/canon_reader.h>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>

namespace bpt::canon::test {

namespace {

const char* kCanonPath = "/tmp/bpt-canon-test/hl-2026-04-25-095045.canon";

bool file_exists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

}  // namespace

TEST(CanonReaderSmoke, ReadsRealReplayOutput) {
    if (!file_exists(kCanonPath)) {
        GTEST_SKIP() << kCanonPath << " not present — run bpt-canon-replay first";
    }

    CanonReader r(kCanonPath);
    ASSERT_TRUE(r.ok());

    const auto& hdr = r.header();
    EXPECT_EQ(0, std::memcmp(hdr.producer_kind, "wslog-replay", 12));

    uint64_t n = 0;
    uint64_t last_ts = 0;
    bool monotonic = true;
    while (auto rec = r.next()) {
        if (rec->ts_ns < last_ts)
            monotonic = false;
        last_ts = rec->ts_ns;
        ++n;
    }
    EXPECT_GT(n, 50000U) << "expected at least 50k events; got " << n;
    EXPECT_TRUE(monotonic) << "ts_ns must be monotonic — wslog is recorded in order";
}

}  // namespace bpt::canon::test
