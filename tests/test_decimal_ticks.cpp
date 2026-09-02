// footprint.md §2.1/§2.2 "integer tick arithmetic throughout; no
// floating-point price bucketing" - golden tests for the exact
// decimal-string parsing/conversion utilities.

#include <gtest/gtest.h>

#include "providers/footprint/decimal_ticks.h"

using namespace truetest::footprint;

// --- parse_decimal ---

TEST(DecimalTicks, ParsesPlainInteger)
{
    auto v = parse_decimal("100");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->mantissa, 100);
    EXPECT_EQ(v->scale, 0);
}

TEST(DecimalTicks, ParsesDecimalWithFraction)
{
    auto v = parse_decimal("27045.10");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->mantissa, 2704510);
    EXPECT_EQ(v->scale, 2);
}

TEST(DecimalTicks, PreservesTrailingZerosAsScale)
{
    auto v = parse_decimal("0.00010000");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->mantissa, 10000);
    EXPECT_EQ(v->scale, 8);
}

TEST(DecimalTicks, ParsesNegative)
{
    auto v = parse_decimal("-3.5");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->mantissa, -35);
    EXPECT_EQ(v->scale, 1);
}

TEST(DecimalTicks, ParsesLeadingPlus)
{
    auto v = parse_decimal("+3.5");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->mantissa, 35);
}

TEST(DecimalTicks, RejectsEmpty)
{
    EXPECT_FALSE(parse_decimal("").has_value());
}

TEST(DecimalTicks, RejectsSignOnly)
{
    EXPECT_FALSE(parse_decimal("-").has_value());
    EXPECT_FALSE(parse_decimal("+").has_value());
}

TEST(DecimalTicks, RejectsMultipleDots)
{
    EXPECT_FALSE(parse_decimal("1.2.3").has_value());
}

TEST(DecimalTicks, RejectsNonDigitCharacters)
{
    EXPECT_FALSE(parse_decimal("12a3").has_value());
    EXPECT_FALSE(parse_decimal("NaN").has_value());
    EXPECT_FALSE(parse_decimal("1e10").has_value());
}

TEST(DecimalTicks, RejectsDotWithNoDigits)
{
    EXPECT_FALSE(parse_decimal(".").has_value());
}

TEST(DecimalTicks, AcceptsLeadingDotAsZeroPointSomething)
{
    // ".5" has no digits before the dot but does have digits after -
    // seen_digit becomes true from the fractional digit, which is correct.
    auto v = parse_decimal(".5");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->mantissa, 5);
    EXPECT_EQ(v->scale, 1);
}

// --- decimal_to_ticks ---

TEST(DecimalTicks, ExactDivisionSameScale)
{
    // price=100.50, tick=0.50 -> exactly 201 ticks.
    auto price = parse_decimal("100.50");
    auto tick = parse_decimal("0.50");
    ASSERT_TRUE(price && tick);
    auto ticks = decimal_to_ticks(*price, *tick);
    ASSERT_TRUE(ticks.has_value());
    EXPECT_EQ(*ticks, 201);
}

TEST(DecimalTicks, ExactDivisionDifferentScales)
{
    // price="27045.1" (scale 1), tick="0.01" (scale 2) -> 2704510 ticks.
    auto price = parse_decimal("27045.1");
    auto tick = parse_decimal("0.01");
    ASSERT_TRUE(price && tick);
    auto ticks = decimal_to_ticks(*price, *tick);
    ASSERT_TRUE(ticks.has_value());
    EXPECT_EQ(*ticks, 2704510);
}

TEST(DecimalTicks, RejectsUnalignedPriceAboveHalfTick)
{
    // price=100.07, tick=0.1 -> 100.07/0.1 = 1000.7 -> rounds to 1001.
    auto price = parse_decimal("100.07");
    auto tick = parse_decimal("0.1");
    ASSERT_TRUE(price && tick);
    auto ticks = decimal_to_ticks(*price, *tick);
    EXPECT_FALSE(ticks.has_value());
}

TEST(DecimalTicks, RejectsUnalignedPriceBelowHalfTick)
{
    // 100.04 / 0.1 = 1000.4 -> rounds to 1000.
    auto price = parse_decimal("100.04");
    auto tick = parse_decimal("0.1");
    ASSERT_TRUE(price && tick);
    auto ticks = decimal_to_ticks(*price, *tick);
    EXPECT_FALSE(ticks.has_value());
}

TEST(DecimalTicks, RejectsZeroTickSize)
{
    auto price = parse_decimal("100.0");
    auto tick = parse_decimal("0");
    ASSERT_TRUE(price && tick);
    EXPECT_FALSE(decimal_to_ticks(*price, *tick).has_value());
}

TEST(DecimalTicks, RejectsNegativeTickSize)
{
    auto price = parse_decimal("100.0");
    auto tick = parse_decimal("-0.1");
    ASSERT_TRUE(price && tick);
    EXPECT_FALSE(decimal_to_ticks(*price, *tick).has_value());
}

TEST(DecimalTicks, HandlesLargeRealisticPrice)
{
    // BTC-scale price, satoshi-scale tick.
    auto price = parse_decimal("68120.50");
    auto tick = parse_decimal("0.01");
    ASSERT_TRUE(price && tick);
    auto ticks = decimal_to_ticks(*price, *tick);
    ASSERT_TRUE(ticks.has_value());
    EXPECT_EQ(*ticks, 6812050);
}

TEST(DecimalTicks, RejectsPathologicalScaleSpread)
{
    // Scale spread far beyond any real venue's precision must be refused,
    // not silently overflow/wrap.
    DecimalValue price{1, 0};
    DecimalValue tick{1, 30};
    EXPECT_FALSE(decimal_to_ticks(price, tick).has_value());
}

TEST(DecimalTicks, ExtremeMantissasRejectWithoutSignedOverflow)
{
    EXPECT_FALSE(decimal_to_ticks(
        DecimalValue{std::numeric_limits<std::int64_t>::max(), 0},
        DecimalValue{2, 0}).has_value());
    EXPECT_FALSE(decimal_to_ticks(
        DecimalValue{std::numeric_limits<std::int64_t>::min(), 0},
        DecimalValue{1, 0}).has_value());
}

// --- decimal_to_atoms ---

TEST(DecimalTicks, AtomsExactWhenIncreasingPrecision)
{
    // qty="1.5" (scale 1) at atom_decimals=8 -> 150000000, exact.
    auto qty = parse_decimal("1.5");
    ASSERT_TRUE(qty.has_value());
    auto atoms = decimal_to_atoms(*qty, 8);
    ASSERT_TRUE(atoms.has_value());
    EXPECT_EQ(*atoms, 150000000);
}

TEST(DecimalTicks, AtomsExactWhenScaleMatches)
{
    auto qty = parse_decimal("0.00000001"); // scale 8
    ASSERT_TRUE(qty.has_value());
    auto atoms = decimal_to_atoms(*qty, 8);
    ASSERT_TRUE(atoms.has_value());
    EXPECT_EQ(*atoms, 1);
}

TEST(DecimalTicks, AtomsRejectWhenReducingPrecisionWouldRoundUp)
{
    // qty="1.567" at atom_decimals=2 -> 156.7 -> rounds to 157.
    auto qty = parse_decimal("1.567");
    ASSERT_TRUE(qty.has_value());
    auto atoms = decimal_to_atoms(*qty, 2);
    EXPECT_FALSE(atoms.has_value());
}

TEST(DecimalTicks, AtomsRejectWhenReducingPrecisionWouldRoundDown)
{
    auto qty = parse_decimal("1.564");
    ASSERT_TRUE(qty.has_value());
    auto atoms = decimal_to_atoms(*qty, 2);
    EXPECT_FALSE(atoms.has_value());
}

TEST(DecimalTicks, AtomsRejectsNegativeAtomDecimals)
{
    auto qty = parse_decimal("1.5");
    ASSERT_TRUE(qty.has_value());
    EXPECT_FALSE(decimal_to_atoms(*qty, -1).has_value());
}

// --- End-to-end: matches the Binance raw-trade JSON fields exactly ---

TEST(DecimalTicks, EndToEndMatchesRawBinanceTradeFields)
{
    // {"e":"trade","p":"68120.50","q":"0.01230000", ...}
    auto price = parse_decimal("68120.50");
    auto qty = parse_decimal("0.01230000");
    ASSERT_TRUE(price && qty);

    auto tick_size = parse_decimal("0.01"); // instrument metadata, exact
    ASSERT_TRUE(tick_size.has_value());

    auto ticks = decimal_to_ticks(*price, *tick_size);
    auto atoms = decimal_to_atoms(*qty, 8);
    ASSERT_TRUE(ticks.has_value());
    ASSERT_TRUE(atoms.has_value());
    EXPECT_EQ(*ticks, 6812050);
    EXPECT_EQ(*atoms, 1230000);
}
