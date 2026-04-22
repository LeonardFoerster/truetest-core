#include <gtest/gtest.h>
#include "data/date_parse.h"

#include <chrono>

using tp = std::chrono::system_clock::time_point;
using ms = std::chrono::milliseconds;
using sec = std::chrono::seconds;

TEST(DateParse, EmptyReturnsNullopt)
{
    EXPECT_FALSE(tt::date_parse::parse(std::string{}).has_value());
    EXPECT_FALSE(tt::date_parse::parse("   ").has_value());
}

TEST(DateParse, EpochMillisThirteenDigits)
{
    // 2024-01-01T00:00:00Z = 1704067200000
    auto r = tt::date_parse::parse("1704067200000");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->time_since_epoch(), ms(1704067200000LL));
}

TEST(DateParse, EpochSecondsTenDigits)
{
    auto r = tt::date_parse::parse("1704067200");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->time_since_epoch(), sec(1704067200));
}

TEST(DateParse, IsoDateOnly_MidnightUtc)
{
    auto r = tt::date_parse::parse("2024-01-01");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->time_since_epoch(), sec(1704067200));
}

TEST(DateParse, IsoDateTimeWithT)
{
    auto r = tt::date_parse::parse("2024-01-01T12:30:45");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->time_since_epoch(), sec(1704067200 + 12 * 3600 + 30 * 60 + 45));
}

TEST(DateParse, IsoDateTimeWithSpace)
{
    auto r = tt::date_parse::parse("2024-01-01 12:30:45");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->time_since_epoch(), sec(1704067200 + 12 * 3600 + 30 * 60 + 45));
}

TEST(DateParse, IsoDateTimeWithFractionalMillis)
{
    auto r = tt::date_parse::parse("2024-01-01T00:00:00.123");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->time_since_epoch(), ms(1704067200000LL + 123));
}

TEST(DateParse, IsoDateTimeWithTrailingZ)
{
    auto r = tt::date_parse::parse("2024-01-01T00:00:00.500Z");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->time_since_epoch(), ms(1704067200000LL + 500));
}

TEST(DateParse, GarbageReturnsNullopt)
{
    EXPECT_FALSE(tt::date_parse::parse("not-a-date").has_value());
    EXPECT_FALSE(tt::date_parse::parse("2024/01/01").has_value());
    EXPECT_FALSE(tt::date_parse::parse("20240101").has_value());
}

TEST(DateParse, MonotonicityUseCase)
{
    // Three bars: distinct dates must be strictly increasing.
    auto a = tt::date_parse::parse("2024-01-01");
    auto b = tt::date_parse::parse("2024-01-02");
    auto c = tt::date_parse::parse("2024-01-02T12:00:00");
    ASSERT_TRUE(a && b && c);
    EXPECT_LT(*a, *b);
    EXPECT_LT(*b, *c);
}
