// Basic coverage for set_thread_name — the helper's value is observational
// (ps -L / /proc/self/comm), so we verify via /proc/self/task/<tid>/comm
// that what we asked for is actually what the kernel stored.

#include <bpt_common/util/thread_name.h>
#include <fstream>
#include <gtest/gtest.h>
#include <pthread.h>
#include <sstream>
#include <string>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

namespace {

// Read /proc/self/task/<tid>/comm for the calling thread. The kernel
// strips the trailing newline, but /proc returns it with one appended.
std::string read_my_thread_name() {
    pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
    std::ostringstream path;
    path << "/proc/self/task/" << tid << "/comm";
    std::ifstream f(path.str());
    std::string s;
    std::getline(f, s);
    return s;
}

}  // namespace

TEST(ThreadNameTest, SetsShortName) {
    // Run in a thread so we don't clobber the test runner's main thread name.
    std::string observed;
    std::thread t([&] {
        bpt::common::util::set_thread_name("hello-thread");
        observed = read_my_thread_name();
    });
    t.join();
    EXPECT_EQ(observed, "hello-thread");
}

TEST(ThreadNameTest, TruncatesAt15Chars) {
    // 20-char input → should truncate to exactly 15.
    std::string observed;
    std::thread t([&] {
        bpt::common::util::set_thread_name("abcdefghijklmnopqrst");
        observed = read_my_thread_name();
    });
    t.join();
    EXPECT_EQ(observed, "abcdefghijklmno");
    EXPECT_EQ(observed.size(), 15u);
}

TEST(ThreadNameTest, EmptyNameIsNoop) {
    // Whatever name the thread had before the empty call should persist.
    std::string before, after;
    std::thread t([&] {
        bpt::common::util::set_thread_name("first-name");
        before = read_my_thread_name();
        bpt::common::util::set_thread_name("");  // no-op
        after = read_my_thread_name();
    });
    t.join();
    EXPECT_EQ(before, "first-name");
    EXPECT_EQ(after, "first-name");
}

TEST(ThreadNameTest, ExactlyFifteenCharsPreservedWithoutTruncation) {
    std::string observed;
    std::thread t([&] {
        bpt::common::util::set_thread_name("exactly15-chars");  // 15 chars
        observed = read_my_thread_name();
    });
    t.join();
    EXPECT_EQ(observed, "exactly15-chars");
    EXPECT_EQ(observed.size(), 15u);
}
