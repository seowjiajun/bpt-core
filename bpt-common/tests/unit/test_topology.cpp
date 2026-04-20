#include <bpt_common/util/topology.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {

// Write `content` to a uniquely-named temp file and return its path.
// Caller is responsible for cleanup (test destructor removes everything
// under the per-test tmp dir).
std::string write_tmp(const std::string& basename, const std::string& content) {
    const auto dir = std::filesystem::temp_directory_path() / "bpt-topology-test";
    std::filesystem::create_directories(dir);
    const auto path = dir / basename;
    std::ofstream f(path);
    f << content;
    return path.string();
}

int nproc() {
    return static_cast<int>(::sysconf(_SC_NPROCESSORS_ONLN));
}

}  // namespace

TEST(TopologyTest, EmptyPathYieldsEmptyTopology) {
    auto topo = bpt::common::util::Topology::load("");
    EXPECT_TRUE(topo.empty());
    EXPECT_EQ(topo.assignment_count(), 0u);
    EXPECT_FALSE(topo.core_for("any.role"));
}

TEST(TopologyTest, EmptyTomlYieldsEmptyTopology) {
    const auto p = write_tmp("empty.toml", "");
    auto topo = bpt::common::util::Topology::load(p);
    EXPECT_TRUE(topo.empty());
}

TEST(TopologyTest, LoadsValidAssignments) {
    const std::string content =
        "[[assignment]]\nrole = \"mdgw.okx.io\"\ncore = 0\n"
        "[[assignment]]\nrole = \"ogw.poll\"\ncore = 1\n";
    const auto p = write_tmp("valid.toml", content);
    auto topo = bpt::common::util::Topology::load(p);
    EXPECT_EQ(topo.assignment_count(), 2u);
    EXPECT_EQ(topo.core_for("mdgw.okx.io"), 0);
    EXPECT_EQ(topo.core_for("ogw.poll"),    1);
    EXPECT_FALSE(topo.core_for("missing.role"));
}

TEST(TopologyTest, RejectsDuplicateCore) {
    const std::string content =
        "[[assignment]]\nrole = \"a\"\ncore = 0\n"
        "[[assignment]]\nrole = \"b\"\ncore = 0\n";
    const auto p = write_tmp("dup_core.toml", content);
    EXPECT_THROW(bpt::common::util::Topology::load(p), std::runtime_error);
}

TEST(TopologyTest, RejectsDuplicateRole) {
    const std::string content =
        "[[assignment]]\nrole = \"a\"\ncore = 0\n"
        "[[assignment]]\nrole = \"a\"\ncore = 1\n";
    const auto p = write_tmp("dup_role.toml", content);
    EXPECT_THROW(bpt::common::util::Topology::load(p), std::runtime_error);
}

TEST(TopologyTest, RejectsCoreOutOfRange) {
    const std::string content =
        "[[assignment]]\nrole = \"a\"\ncore = " + std::to_string(nproc() + 100) + "\n";
    const auto p = write_tmp("oor.toml", content);
    EXPECT_THROW(bpt::common::util::Topology::load(p), std::runtime_error);
}

TEST(TopologyTest, RejectsNegativeCore) {
    const std::string content =
        "[[assignment]]\nrole = \"a\"\ncore = -1\n";
    const auto p = write_tmp("neg.toml", content);
    EXPECT_THROW(bpt::common::util::Topology::load(p), std::runtime_error);
}

TEST(TopologyTest, RejectsTotalCoresMismatch) {
    // host.total_cores deliberately wrong — should fail even before we
    // look at assignments so the operator catches "wrong topology file"
    // errors at the earliest possible point.
    const std::string content =
        "[host]\ntotal_cores = " + std::to_string(nproc() + 42) + "\n";
    const auto p = write_tmp("mismatch.toml", content);
    EXPECT_THROW(bpt::common::util::Topology::load(p), std::runtime_error);
}

TEST(TopologyTest, AcceptsMatchingTotalCores) {
    const std::string content =
        "[host]\ntotal_cores = " + std::to_string(nproc()) + "\n"
        "[[assignment]]\nrole = \"a\"\ncore = 0\n";
    const auto p = write_tmp("match.toml", content);
    auto topo = bpt::common::util::Topology::load(p);
    EXPECT_EQ(topo.assignment_count(), 1u);
    EXPECT_EQ(topo.core_for("a"), 0);
}

TEST(TopologyTest, RejectsMissingRoleField) {
    const std::string content = "[[assignment]]\ncore = 0\n";
    const auto p = write_tmp("no_role.toml", content);
    EXPECT_THROW(bpt::common::util::Topology::load(p), std::runtime_error);
}

TEST(TopologyTest, RejectsMissingCoreField) {
    const std::string content = "[[assignment]]\nrole = \"x\"\n";
    const auto p = write_tmp("no_core.toml", content);
    EXPECT_THROW(bpt::common::util::Topology::load(p), std::runtime_error);
}

TEST(TopologyTest, RejectsMalformedToml) {
    const auto p = write_tmp("bad.toml", "this is not = valid = toml [\n");
    EXPECT_THROW(bpt::common::util::Topology::load(p), std::runtime_error);
}
