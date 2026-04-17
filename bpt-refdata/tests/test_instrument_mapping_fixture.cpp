// End-to-end contract test: loads the actual JSON produced by bpt-ops
// (bpt-ops instrument-mapping --exchange OKX) through InstrumentMappingLoader
// and asserts the canonical IDs for seeded pairs are where we expect them.
//
// This is what catches producer/consumer drift: if bpt-ops ever emits a shape
// refdata's loader can't parse, this test fails before the mapping reaches
// production.

#include "refdata/mapping/instrument_mapping_loader.h"

#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>

using namespace bpt::refdata::mapping;

namespace {

std::string runfile_path(const std::string& relative) {
    // Bazel exposes data deps under the test's runfiles tree. Walk upward until
    // we find the file (bazel-bin layout nests the runfiles dir a few levels
    // deep and this avoids depending on Bazel runfiles helpers just for a fixture).
    namespace fs = std::filesystem;
    for (auto p = fs::current_path(); !p.empty(); p = p.parent_path()) {
        const auto candidate = p / relative;
        if (fs::exists(candidate))
            return candidate.string();
        if (p == p.root_path())
            break;
    }
    return {};
}

}  // namespace

TEST(InstrumentMappingFixture, LoadsGeneratedOkxFile) {
    const auto path = runfile_path("config/instruments/instrument_mapping.okx.json");
    if (path.empty())
        GTEST_SKIP() << "generated mapping not present; run bpt-ops instrument-mapping first";

    InstrumentMappingLoader loader;
    ASSERT_NO_THROW(loader.load(path));

    // Seeds must be where we pinned them.
    EXPECT_EQ(loader.try_resolve_canonical_id(EXCHANGE_ID_OKX, "BTC-USDT-SWAP"), 1001u);
    EXPECT_EQ(loader.try_resolve_canonical_id(EXCHANGE_ID_OKX, "ETH-USDT-SWAP"), 1002u);
    EXPECT_EQ(loader.try_resolve_canonical_id(EXCHANGE_ID_OKX, "SOL-USDT-SWAP"), 1003u);
    EXPECT_EQ(loader.try_resolve_canonical_id(EXCHANGE_ID_OKX, "BTC-USDT"), 2001u);
    EXPECT_EQ(loader.try_resolve_canonical_id(EXCHANGE_ID_OKX, "ETH-USDT"), 2002u);
    EXPECT_EQ(loader.try_resolve_canonical_id(EXCHANGE_ID_OKX, "SOL-USDT"), 2003u);

    // Non-seed instruments land in their type bucket with monotonic IDs.
    auto info_2001 = loader.get_instrument_info(2001);
    ASSERT_TRUE(info_2001.has_value());
    EXPECT_EQ(info_2001->base, "BTC");
    EXPECT_EQ(info_2001->quote, "USDT");
    EXPECT_EQ(info_2001->type, "SPOT");

    // The OKX fetch returns hundreds of live instruments; sanity-check the count.
    EXPECT_GT(loader.instrument_count(), 100u);
}
