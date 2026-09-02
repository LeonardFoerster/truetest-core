#include <gtest/gtest.h>
#include "data/date_parse.h"
#include "helpers/alloc_counter.h"

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

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

TEST(DateParse, RejectsAmbiguousEpochUnitsAndChronoOverflow)
{
    for (const std::string_view value :
         {"170406720000",         // 12 digits: no declared unit
          "17040672000000",       // 14 digits: no declared unit
          "1704067200000000",     // microseconds must not be guessed as ms
          "1704067200000000000",  // nanoseconds must not be guessed as ms
          "9999999999999",        // ms value outside system_clock range
          "999999999999999999999999999999999999"})
        EXPECT_FALSE(tt::date_parse::parse(value).has_value()) << value;
}

TEST(DateParse, RejectsEpochZero)
{
    EXPECT_FALSE(tt::date_parse::parse("0000000000").has_value());
    EXPECT_FALSE(tt::date_parse::parse("0000000000000").has_value());
}

TEST(DateParse, RejectsInvalidCivilDatesAndTimes)
{
    for (const std::string_view value :
         {"2023-02-29", "2024-02-30", "2024-00-01", "2024-13-01", "2024-01-00", "2024-01-32",
          "2024-01-01T24:00:00", "2024-01-01T25:61:61", "2024-01-01T00:60:00",
          "2024-01-01T00:00:60"})
        EXPECT_FALSE(tt::date_parse::parse(value).has_value()) << value;

    EXPECT_TRUE(tt::date_parse::parse("2000-02-29").has_value());
    EXPECT_FALSE(tt::date_parse::parse("2100-02-29").has_value());
}

TEST(DateParse, RequiresCompleteIsoToken)
{
    for (const std::string_view value :
         {"2024-01-01junk", "2024-01-01T00:00:00junk", "2024-01-01T00:00:00.1234", "2024-01-01Z",
          "2024-01-01T00", "2024-01-01T00:00:00ZZ"})
        EXPECT_FALSE(tt::date_parse::parse(value).has_value()) << value;
}

TEST(DateParse, ExactHourMinuteFormRemainsSupported)
{
    const auto minute = tt::date_parse::parse("2024-01-01T12:34");
    const auto second = tt::date_parse::parse("2024-01-01T12:34:00");
    ASSERT_TRUE(minute && second);
    EXPECT_EQ(*minute, *second);
}

TEST(DateParse, FractionalSecondDigitsHaveDecimalMeaning)
{
    const auto one_digit = tt::date_parse::parse("2024-01-01T00:00:00.5Z");
    const auto two_digits = tt::date_parse::parse("2024-01-01T00:00:00.05Z");
    const auto three_digits = tt::date_parse::parse("2024-01-01T00:00:00.005Z");
    ASSERT_TRUE(one_digit && two_digits && three_digits);
    EXPECT_EQ(one_digit->time_since_epoch(), ms(1704067200500LL));
    EXPECT_EQ(two_digits->time_since_epoch(), ms(1704067200050LL));
    EXPECT_EQ(three_digits->time_since_epoch(), ms(1704067200005LL));
}

TEST(DateParse, AppliesExplicitUtcOffsets)
{
    const auto utc = tt::date_parse::parse("2024-01-01T00:00:00Z");
    const auto plus_two = tt::date_parse::parse("2024-01-01T02:00:00+02:00");
    const auto minus_two = tt::date_parse::parse("2023-12-31T22:00:00-02:00");
    ASSERT_TRUE(utc && plus_two && minus_two);
    EXPECT_EQ(*plus_two, *utc);
    EXPECT_EQ(*minus_two, *utc);
}

TEST(DateParse, RejectsMalformedUtcOffsets)
{
    for (const std::string_view value :
         {"2024-01-01T00:00:00+2:00", "2024-01-01T00:00:00+0200", "2024-01-01T00:00:00+24:00",
          "2024-01-01T00:00:00+01:60", "2024-01-01T00:00:00+14:01",
          "2024-01-01T00:00:00+02:00junk"})
        EXPECT_FALSE(tt::date_parse::parse(value).has_value()) << value;
}

TEST(DateParse, RejectsSignedFixedWidthCivilComponents)
{
    for (const std::string_view value :
         {"2024-01-01T-1:00:00", "2024-01-01T00:-1:00", "2024-01-01T00:00:-1",
          "2024-01-01T-0:00:00", "2024-01-01T00:-0:00", "2024-01-01T00:00:-0",
          "2024-01-01T00:00:00+-1:00", "2024-01-01T00:00:00+00:-1", "2024-01-01T00:00:00--1:00",
          "2024-01-01T00:00:00+-0:00"})
        EXPECT_FALSE(tt::date_parse::parse(value).has_value()) << value;
}

TEST(DateParse, CheckedEpochConversionAcceptsBoundaryAndRejectsNextValue)
{
    using target_duration = std::chrono::system_clock::duration;
    using conversion = std::ratio_divide<std::milli, target_duration::period>;
    static_assert(conversion::den == 1);
    const auto maximum_milliseconds =
        static_cast<std::int64_t>(target_duration::max().count() / conversion::num);
    ASSERT_GT(maximum_milliseconds, 0);
    EXPECT_TRUE(tt::date_parse::from_epoch_milliseconds(maximum_milliseconds).has_value());
    ASSERT_LT(maximum_milliseconds, std::numeric_limits<std::int64_t>::max());
    EXPECT_FALSE(tt::date_parse::from_epoch_milliseconds(maximum_milliseconds + 1).has_value());
}

TEST(DateParse, ExplicitInvalidOpenTimeNeverFallsBackToDate)
{
    const auto date = tt::date_parse::parse("2024-01-01");
    ASSERT_TRUE(date.has_value());

    EXPECT_EQ(tt::date_parse::try_resolve_bar_clock(std::nullopt, "2024-01-01"), date);
    EXPECT_FALSE(tt::date_parse::try_resolve_bar_clock(std::optional<std::int64_t>{0}, "2024-01-01")
                     .has_value());
    EXPECT_FALSE(tt::date_parse::try_resolve_bar_clock(std::optional<std::int64_t>{9999999999999LL},
                                                       "2024-01-01")
                     .has_value());
}

TEST(DateParse, ParsingDoesNotAllocateAfterInputConstruction)
{
    const std::string_view input = "2024-02-29T23:59:59.123+01:30";
    bool all_valid = true;
    truetest::test::alloc::measure_window measured;
    for (std::size_t index = 0; index < 100000; ++index)
        all_valid = tt::date_parse::parse(input).has_value() && all_valid;
    const auto allocations = measured.total();

    EXPECT_TRUE(all_valid);
    EXPECT_EQ(allocations.count, 0U);
    EXPECT_EQ(allocations.bytes, 0U);
}
