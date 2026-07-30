#ifdef HAS_QUESTDB

#include "data/questdb/run_tag.h"

#include <gtest/gtest.h>

#include <regex>
#include <stdexcept>
#include <thread>

using truetest::questdb::is_valid_run_tag;
using truetest::questdb::make_run_tag;

TEST(QuestdbRunTag, GeneratedTagMatchesExpectedFormat)
{
    const std::string tag = make_run_tag("");
    static const std::regex pattern(R"(^run_\d{8}_\d{6}_[0-9a-f]{6}$)");
    EXPECT_TRUE(std::regex_match(tag, pattern)) << "tag=" << tag;
}

TEST(QuestdbRunTag, OverridePassesThrough)
{
    EXPECT_EQ(make_run_tag("experiment_v3"), "experiment_v3");
    EXPECT_EQ(make_run_tag("ABC_123"), "ABC_123");
}

TEST(QuestdbRunTag, InvalidOverrideThrows)
{
    EXPECT_THROW(make_run_tag("bad tag!"), std::invalid_argument);
    EXPECT_THROW(make_run_tag("with-dash"), std::invalid_argument);
    EXPECT_THROW(make_run_tag(std::string(65, 'a')), std::invalid_argument);
}

TEST(QuestdbRunTag, EmptyOverrideWithSameSeedIsDeterministicSuffix)
{
    // Same seed -> same hex suffix (last 6 chars). Wall-clock prefix may
    // tick between calls but typically does not.
    const std::string a = make_run_tag("", /*test_seed=*/42);
    const std::string b = make_run_tag("", /*test_seed=*/42);
    EXPECT_EQ(a.substr(a.size() - 6), b.substr(b.size() - 6));
}

TEST(QuestdbRunTag, TwoUnseededCallsLikelyDiffer)
{
    const std::string a = make_run_tag("");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const std::string b = make_run_tag("");
    // Random suffix makes a collision astronomically unlikely.
    EXPECT_NE(a, b);
}

TEST(QuestdbRunTag, IsValidRunTag)
{
    EXPECT_TRUE(is_valid_run_tag("abc"));
    EXPECT_TRUE(is_valid_run_tag("run_20260424_120000_abcdef"));
    EXPECT_FALSE(is_valid_run_tag(""));
    EXPECT_FALSE(is_valid_run_tag("a-b"));
    EXPECT_FALSE(is_valid_run_tag(std::string(65, 'a')));
}

#endif // HAS_QUESTDB
