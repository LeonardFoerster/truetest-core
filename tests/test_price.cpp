#include <gtest/gtest.h>
#include "types/price.h"

TEST(Price, DefaultZero)
{
    Price p;
    EXPECT_EQ(p.raw(), 0);
    EXPECT_DOUBLE_EQ(p.to_double(), 0.0);
}

TEST(Price, FromDouble)
{
    Price p = Price::from_double(100.0);
    EXPECT_EQ(p.raw(), 1000000);
    EXPECT_DOUBLE_EQ(p.to_double(), 100.0);
}

TEST(Price, FromDoubleSubCent)
{
    Price p = Price::from_double(99.9999);
    EXPECT_EQ(p.raw(), 999999);
    EXPECT_DOUBLE_EQ(p.to_double(), 99.9999);
}

TEST(Price, RoundTrip)
{
    double values[] = {0.0001, 1.0, 42.5, 100.25, 99999.9999, 0.0};
    for (double v : values)
    {
        Price p = Price::from_double(v);
        EXPECT_DOUBLE_EQ(p.to_double(), v) << "Failed for " << v;
    }
}

TEST(Price, Arithmetic)
{
    Price a = Price::from_double(10.0);
    Price b = Price::from_double(3.5);
    EXPECT_DOUBLE_EQ((a + b).to_double(), 13.5);
    EXPECT_DOUBLE_EQ((a - b).to_double(), 6.5);
    EXPECT_DOUBLE_EQ((-a).to_double(), -10.0);
}

TEST(Price, CompoundAssignment)
{
    Price a = Price::from_double(10.0);
    Price b = Price::from_double(3.0);
    a += b;
    EXPECT_DOUBLE_EQ(a.to_double(), 13.0);
    a -= Price::from_double(5.0);
    EXPECT_DOUBLE_EQ(a.to_double(), 8.0);
}

TEST(Price, Comparison)
{
    Price a = Price::from_double(10.0);
    Price b = Price::from_double(20.0);
    Price c = Price::from_double(10.0);

    EXPECT_TRUE(a < b);
    EXPECT_TRUE(b > a);
    EXPECT_TRUE(a == c);
    EXPECT_TRUE(a != b);
    EXPECT_TRUE(a <= c);
    EXPECT_TRUE(a <= b);
    EXPECT_TRUE(b >= a);
    EXPECT_TRUE(a >= c);
}

TEST(Price, Negative)
{
    Price p = Price::from_double(-50.25);
    EXPECT_DOUBLE_EQ(p.to_double(), -50.25);
    EXPECT_LT(p, Price());
}

TEST(Price, LargeValues)
{
    Price p = Price::from_double(999999999.9999);
    EXPECT_DOUBLE_EQ(p.to_double(), 999999999.9999);
}

TEST(Price, CheckedConversionRejectsRoundedIntegerOverflow)
{
    Price out;
    EXPECT_TRUE(Price::try_from_double(100.25, out));
    EXPECT_DOUBLE_EQ(out.to_double(), 100.25);

    const double positive_edge =
        static_cast<double>(std::numeric_limits<int64_t>::max())
        / static_cast<double>(Price::SCALE);
    EXPECT_FALSE(Price::try_from_double(
        std::nextafter(positive_edge,
                       std::numeric_limits<double>::infinity()), out));
    EXPECT_FALSE(Price::try_from_double(1e100, out));
    EXPECT_FALSE(Price::try_from_double(
        std::numeric_limits<double>::quiet_NaN(), out));
}
