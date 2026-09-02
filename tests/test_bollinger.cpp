#include <gtest/gtest.h>
#include "indicator/bollinger.h"
#include "helpers/alloc_counter.h"

#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace {

struct StablePopulationStats
{
    long double mean = 0.0L;
    long double variance = 0.0L;
};

StablePopulationStats reference_population_stats(const std::vector<double>& values,
                                                 std::size_t begin, std::size_t count)
{
    long double mean = 0.0L;
    for (std::size_t i = 0; i < count; ++i)
        mean += static_cast<long double>(values[begin + i]);
    mean /= static_cast<long double>(count);

    long double squared_deviations = 0.0L;
    for (std::size_t i = 0; i < count; ++i) {
        const long double deviation = static_cast<long double>(values[begin + i]) - mean;
        squared_deviations += deviation * deviation;
    }
    return {mean, squared_deviations / static_cast<long double>(count)};
}

}  // namespace

static_assert(std::is_copy_constructible_v<bollinger_bands>);
static_assert(std::is_copy_assignable_v<bollinger_bands>);
static_assert(std::is_move_constructible_v<bollinger_bands>);
static_assert(std::is_move_assignable_v<bollinger_bands>);

TEST(Bollinger, WarmupPeriod)
{
    bollinger_bands bb(3);
    EXPECT_EQ(bb.update(10.0), std::nullopt);
    EXPECT_EQ(bb.update(20.0), std::nullopt);
    EXPECT_FALSE(bb.ready());
}

TEST(Bollinger, FirstValue)
{
    bollinger_bands bb(3, 2.0);
    bb.update(10.0);
    bb.update(20.0);
    auto val = bb.update(30.0);
    ASSERT_TRUE(val.has_value());
    EXPECT_DOUBLE_EQ(val->middle, 20.0);
    // stddev = sqrt(((100+400+900)/3) - 400) = sqrt(200/3) ≈ 8.165
    double expected_std = std::sqrt(200.0 / 3.0);
    EXPECT_NEAR(val->upper, 20.0 + 2.0 * expected_std, 0.001);
    EXPECT_NEAR(val->lower, 20.0 - 2.0 * expected_std, 0.001);
}

TEST(Bollinger, ConstantInput)
{
    bollinger_bands bb(3, 2.0);
    bb.update(5.0);
    bb.update(5.0);
    auto val = bb.update(5.0);
    ASSERT_TRUE(val.has_value());
    EXPECT_DOUBLE_EQ(val->middle, 5.0);
    // stddev = 0 -> bands collapse to middle
    EXPECT_DOUBLE_EQ(val->upper, 5.0);
    EXPECT_DOUBLE_EQ(val->lower, 5.0);
}

TEST(Bollinger, SlidingWindow)
{
    bollinger_bands bb(3, 1.0);
    bb.update(10.0);
    bb.update(20.0);
    bb.update(30.0);
    auto val = bb.update(40.0);  // window: 20, 30, 40
    ASSERT_TRUE(val.has_value());
    EXPECT_DOUBLE_EQ(val->middle, 30.0);
}

TEST(Bollinger, DefaultParams)
{
    bollinger_bands bb;  // period=20, num_std=2
    for (int i = 0; i < 19; ++i)
        bb.update(100.0);
    EXPECT_FALSE(bb.ready());
    auto val = bb.update(100.0);
    ASSERT_TRUE(val.has_value());
    EXPECT_DOUBLE_EQ(val->middle, 100.0);
}

TEST(Bollinger, LargeOffsetPreservesAnalyticVariance)
{
    bollinger_bands bb(3, 2.0);
    bb.update(1.0e12 - 1.0);
    bb.update(1.0e12);
    const auto result = bb.update(1.0e12 + 1.0);

    ASSERT_TRUE(result.has_value());
    const double expected_stddev = std::sqrt(2.0 / 3.0);
    EXPECT_DOUBLE_EQ(result->middle, 1.0e12);
    EXPECT_NEAR(result->upper - result->middle, 2.0 * expected_stddev, 2.0e-4);
    EXPECT_NEAR(result->middle - result->lower, 2.0 * expected_stddev, 2.0e-4);
}

TEST(Bollinger, TranslationPreservesBandWidth)
{
    constexpr double offset = 1.0e12;
    bollinger_bands base(5, 2.0);
    bollinger_bands translated(5, 2.0);
    std::optional<bollinger_result> base_result;
    std::optional<bollinger_result> translated_result;

    for (const double value : {-2.0, -0.5, 0.25, 1.5, 3.0}) {
        base_result = base.update(value);
        translated_result = translated.update(value + offset);
    }

    ASSERT_TRUE(base_result.has_value());
    ASSERT_TRUE(translated_result.has_value());
    EXPECT_NEAR(translated_result->middle - offset, base_result->middle, 2.0e-4);
    EXPECT_NEAR(translated_result->upper - translated_result->middle,
                base_result->upper - base_result->middle, 2.0e-4);
    EXPECT_NEAR(translated_result->middle - translated_result->lower,
                base_result->middle - base_result->lower, 2.0e-4);
}

TEST(Bollinger, ScalingPreservesNormalizedBands)
{
    constexpr double scale = 1.0e6;
    bollinger_bands base(5, 1.75);
    bollinger_bands scaled(5, 1.75);
    std::optional<bollinger_result> base_result;
    std::optional<bollinger_result> scaled_result;

    for (const double value : {1.25, 2.0, 4.5, 8.0, 16.0}) {
        base_result = base.update(value);
        scaled_result = scaled.update(value * scale);
    }

    ASSERT_TRUE(base_result.has_value());
    ASSERT_TRUE(scaled_result.has_value());
    EXPECT_NEAR(scaled_result->middle / scale, base_result->middle, 1.0e-12);
    EXPECT_NEAR(scaled_result->upper / scale, base_result->upper, 1.0e-12);
    EXPECT_NEAR(scaled_result->lower / scale, base_result->lower, 1.0e-12);
}

TEST(Bollinger, SlidingWindowMatchesStableReferenceAtLargeOffset)
{
    constexpr std::size_t period = 31;
    constexpr double offset = 1.0e12;
    bollinger_bands bb(period, 2.0);
    std::vector<double> values;
    values.reserve(5000);

    for (std::size_t i = 0; i < 5000; ++i) {
        // Deterministic boundary-rich generator: a small non-periodic signal
        // around a large offset exercises both removal and addition without
        // copying the implementation's recurrence into the oracle.
        const auto centered = static_cast<long long>((i * 48271U) % 257U) - 128LL;
        const double value = offset + static_cast<double>(centered) * 0.125;
        values.push_back(value);
        const auto result = bb.update(value);

        if (i + 1 < period) {
            EXPECT_FALSE(result.has_value());
            continue;
        }

        ASSERT_TRUE(result.has_value());
        const auto reference = reference_population_stats(values, i + 1 - period, period);
        const long double reference_stddev = std::sqrt(reference.variance);
        EXPECT_NEAR(result->middle, static_cast<double>(reference.mean), 2.0e-4) << "sample " << i;
        EXPECT_NEAR(result->upper - result->middle, static_cast<double>(2.0L * reference_stddev),
                    4.0e-4)
            << "sample " << i;
        EXPECT_NEAR(result->middle - result->lower, static_cast<double>(2.0L * reference_stddev),
                    4.0e-4)
            << "sample " << i;
    }
}

TEST(Bollinger, RejectsInvalidConfiguration)
{
    EXPECT_THROW((void)bollinger_bands(0, 2.0), std::invalid_argument);
    EXPECT_THROW((void)bollinger_bands(20, -1.0), std::invalid_argument);
    EXPECT_THROW((void)bollinger_bands(20, std::numeric_limits<double>::infinity()),
                 std::invalid_argument);
    EXPECT_THROW((void)bollinger_bands(20, std::numeric_limits<double>::quiet_NaN()),
                 std::invalid_argument);
    EXPECT_THROW((void)bollinger_bands(std::numeric_limits<std::size_t>::max(),
                                       std::numeric_limits<double>::quiet_NaN()),
                 std::invalid_argument);
}

TEST(Bollinger, InvalidPriceDoesNotMutateWindow)
{
    bollinger_bands subject(3, 2.0);
    bollinger_bands control(3, 2.0);
    ASSERT_FALSE(subject.update(10.0).has_value());
    ASSERT_FALSE(control.update(10.0).has_value());

    EXPECT_THROW(subject.update(std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
    EXPECT_THROW(subject.update(std::numeric_limits<double>::infinity()), std::invalid_argument);

    ASSERT_FALSE(subject.update(20.0).has_value());
    ASSERT_FALSE(control.update(20.0).has_value());
    const auto subject_result = subject.update(30.0);
    const auto control_result = control.update(30.0);
    ASSERT_TRUE(subject_result.has_value());
    ASSERT_TRUE(control_result.has_value());
    EXPECT_DOUBLE_EQ(subject_result->middle, control_result->middle);
    EXPECT_DOUBLE_EQ(subject_result->upper, control_result->upper);
    EXPECT_DOUBLE_EQ(subject_result->lower, control_result->lower);
}

TEST(Bollinger, PeriodOneAndZeroMultiplierCollapseBands)
{
    bollinger_bands one_sample(1, 2.0);
    const auto one = one_sample.update(123.5);
    ASSERT_TRUE(one.has_value());
    EXPECT_DOUBLE_EQ(one->middle, 123.5);
    EXPECT_DOUBLE_EQ(one->upper, 123.5);
    EXPECT_DOUBLE_EQ(one->lower, 123.5);

    bollinger_bands zero_width(3, 0.0);
    zero_width.update(10.0);
    zero_width.update(20.0);
    const auto zero = zero_width.update(30.0);
    ASSERT_TRUE(zero.has_value());
    EXPECT_DOUBLE_EQ(zero->upper, zero->middle);
    EXPECT_DOUBLE_EQ(zero->lower, zero->middle);
}

TEST(Bollinger, OverflowingFiniteCandidateDoesNotMutateWindow)
{
    bollinger_bands subject(2, 2.0);
    bollinger_bands control(2, 2.0);
    ASSERT_FALSE(subject.update(1.0).has_value());
    ASSERT_FALSE(control.update(1.0).has_value());

    EXPECT_THROW(subject.update(std::numeric_limits<double>::max()), std::overflow_error);

    const auto subject_result = subject.update(3.0);
    const auto control_result = control.update(3.0);
    ASSERT_TRUE(subject_result.has_value());
    ASSERT_TRUE(control_result.has_value());
    EXPECT_DOUBLE_EQ(subject_result->middle, control_result->middle);
    EXPECT_DOUBLE_EQ(subject_result->upper, control_result->upper);
    EXPECT_DOUBLE_EQ(subject_result->lower, control_result->lower);
}

TEST(Bollinger, LongRunRemainsStableAcrossShadowRebases)
{
    constexpr std::size_t period = 20;
    constexpr std::size_t samples = 1'000'000;
    constexpr double offset = 1.0e12;
    bollinger_bands bb(period, 2.0);
    std::vector<double> ring(period);
    std::size_t next = 0;

    for (std::size_t i = 0; i < samples; ++i) {
        const auto centered = static_cast<long long>((i * 16807U + i / 17U) % 1021U) - 510LL;
        const double value = offset + static_cast<double>(centered) * 0.03125;
        ring[next] = value;
        next = (next + 1) % period;
        const auto result = bb.update(value);

        if (i + 1 < period) {
            ASSERT_FALSE(result.has_value());
            continue;
        }
        ASSERT_TRUE(result.has_value());
        if (i % 4093U != 0 && i + 1 != samples) continue;

        const auto reference = reference_population_stats(ring, 0, period);
        const long double reference_width = 2.0L * std::sqrt(reference.variance);
        EXPECT_NEAR(result->middle, static_cast<double>(reference.mean), 2.0e-4) << "sample " << i;
        EXPECT_NEAR(result->upper - result->middle, static_cast<double>(reference_width), 4.0e-4)
            << "sample " << i;
        EXPECT_NEAR(result->middle - result->lower, static_cast<double>(reference_width), 4.0e-4)
            << "sample " << i;
    }
}

TEST(Bollinger, UpdateAllocatesNothingAfterConstruction)
{
    bollinger_bands bb(20, 2.0);
    for (std::size_t i = 0; i < 20; ++i)
        ASSERT_TRUE(bb.update(100.0 + static_cast<double>(i % 7)).has_value() || i + 1 < 20);

    std::optional<bollinger_result> final_result;
    truetest::test::alloc::snapshot allocations;
    {
        truetest::test::alloc::measure_window measured;
        for (std::size_t i = 0; i < 1'000'000; ++i) {
            final_result = bb.update(100.0 + static_cast<double>((i * 48271U) % 101U) * 0.01);
        }
        allocations = measured.total();
    }

    ASSERT_TRUE(final_result.has_value());
    EXPECT_EQ(allocations.count, 0U);
    EXPECT_EQ(allocations.bytes, 0U);
}
