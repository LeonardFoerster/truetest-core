#include <gtest/gtest.h>

#include "analytics/shadow_tracker.h"
#include "helpers/alloc_counter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <type_traits>
#include <vector>

namespace {

using milliseconds = std::chrono::milliseconds;
using time_point = std::chrono::system_clock::time_point;

fill_event make_fill(std::uint64_t order_id, order_side side, double quantity, double price,
                     std::int64_t timestamp_ms, const char* symbol = "BTCUSDT")
{
    return fill_event(time_point{milliseconds{timestamp_ms}}, symbol, order_id, side, quantity,
                      price);
}

using LegacyFillHandler = void (ShadowTracker::*)(const fill_event&);
static_assert(std::is_same_v<decltype(&ShadowTracker::on_simulated_fill), LegacyFillHandler>);
static_assert(std::is_same_v<decltype(&ShadowTracker::on_exchange_fill), LegacyFillHandler>);

}  // namespace

TEST(ShadowTracker, AggregatesPartialFillsIntoPerLegVwap)
{
    ShadowTracker tracker;

    tracker.on_simulated_fill(make_fill(42, order_side::buy, 1.0, 100.0, 10));
    tracker.on_simulated_fill(make_fill(42, order_side::buy, 3.0, 110.0, 20));
    tracker.on_exchange_fill(make_fill(42, order_side::buy, 3.0, 100.0, 15));
    tracker.on_exchange_fill(make_fill(42, order_side::buy, 1.0, 140.0, 30));

    const auto& fill = tracker.fills().at(42);
    EXPECT_DOUBLE_EQ(fill.sim_quantity, 4.0);
    EXPECT_DOUBLE_EQ(fill.sim_price, 107.5);
    EXPECT_DOUBLE_EQ(fill.exchange_quantity, 4.0);
    EXPECT_DOUBLE_EQ(fill.exchange_price, 110.0);
    EXPECT_DOUBLE_EQ(fill.slippage(), 2.5);
}

TEST(ShadowTracker, AggregatesCanonicalEconomicQuantityInsteadOfReportedQuantity)
{
    ShadowTracker tracker;
    auto first = make_fill(7, order_side::sell, 5.0, 101.0, 10);
    first.set_economic_quantity(2.0);
    auto second = make_fill(7, order_side::sell, 8.0, 103.0, 20);
    second.set_economic_quantity(3.0);

    tracker.on_exchange_fill(first);
    tracker.on_exchange_fill(second);

    const auto& fill = tracker.fills().at(7);
    EXPECT_DOUBLE_EQ(fill.exchange_quantity, 5.0);
    EXPECT_DOUBLE_EQ(fill.exchange_price, 102.2);
}

TEST(ShadowTracker, SideNormalizesAdverseSlippage)
{
    ShadowTracker buy_tracker;
    ASSERT_TRUE(buy_tracker.try_on_simulated_fill(make_fill(1, order_side::buy, 2.0, 100.0, 10)));
    ASSERT_TRUE(buy_tracker.try_on_exchange_fill(make_fill(1, order_side::buy, 2.0, 102.0, 20)));
    const auto& buy = buy_tracker.fills().at(1);
    ASSERT_TRUE(buy.comparable_adverse_slippage().has_value());
    EXPECT_DOUBLE_EQ(*buy.comparable_adverse_slippage(), 2.0);
    ASSERT_TRUE(buy.comparable_adverse_slippage_bps().has_value());
    EXPECT_DOUBLE_EQ(*buy.comparable_adverse_slippage_bps(), 200.0);

    ShadowTracker sell_tracker;
    ASSERT_TRUE(sell_tracker.try_on_simulated_fill(make_fill(2, order_side::sell, 2.0, 100.0, 10)));
    ASSERT_TRUE(sell_tracker.try_on_exchange_fill(make_fill(2, order_side::sell, 2.0, 102.0, 20)));
    const auto& sell = sell_tracker.fills().at(2);
    ASSERT_TRUE(sell.comparable_adverse_slippage().has_value());
    EXPECT_DOUBLE_EQ(*sell.comparable_adverse_slippage(), -2.0);
    ASSERT_TRUE(sell.comparable_adverse_slippage_bps().has_value());
    EXPECT_DOUBLE_EQ(*sell.comparable_adverse_slippage_bps(), -200.0);
}

TEST(ShadowTracker, UnequalQuantitiesAreVisibleAndNotPriceComparable)
{
    ShadowTracker tracker;
    ASSERT_TRUE(tracker.try_on_simulated_fill(make_fill(3, order_side::buy, 5.0, 100.0, 10)));
    ASSERT_TRUE(tracker.try_on_exchange_fill(make_fill(3, order_side::buy, 3.0, 102.0, 20)));

    const auto& fill = tracker.fills().at(3);
    EXPECT_DOUBLE_EQ(fill.quantity_divergence(), -2.0);
    EXPECT_FALSE(fill.quantities_match());
    EXPECT_FALSE(fill.comparable_slippage().has_value());
    EXPECT_FALSE(fill.comparable_adverse_slippage_bps().has_value());
}

TEST(ShadowTracker, TinyQuantitiesUseRelativeRatherThanUnitScaleTolerance)
{
    ShadowTracker tracker;
    ASSERT_TRUE(tracker.try_on_simulated_fill(make_fill(13, order_side::buy, 1.0e-20, 100.0, 10)));
    ASSERT_TRUE(tracker.try_on_exchange_fill(make_fill(13, order_side::buy, 2.0e-20, 200.0, 20)));

    const auto& fill = tracker.fills().at(13);
    EXPECT_FALSE(fill.quantities_match());
    EXPECT_FALSE(fill.comparable_slippage().has_value());
    EXPECT_DOUBLE_EQ(fill.quantity_divergence(), 1.0e-20);
}

TEST(ShadowTracker, TracksFirstAndLastEventTimesIndependentOfArrivalOrder)
{
    ShadowTracker tracker;
    ASSERT_TRUE(tracker.try_on_simulated_fill(make_fill(4, order_side::buy, 1.0, 100.0, 30)));
    ASSERT_TRUE(tracker.try_on_simulated_fill(make_fill(4, order_side::buy, 1.0, 100.0, 10)));
    ASSERT_TRUE(tracker.try_on_exchange_fill(make_fill(4, order_side::buy, 1.0, 100.0, 40)));
    ASSERT_TRUE(tracker.try_on_exchange_fill(make_fill(4, order_side::buy, 1.0, 100.0, 20)));

    const auto& fill = tracker.fills().at(4);
    EXPECT_EQ(fill.sim_first_timestamp, time_point{milliseconds{10}});
    EXPECT_EQ(fill.sim_timestamp, time_point{milliseconds{30}});
    EXPECT_EQ(fill.exchange_first_timestamp, time_point{milliseconds{20}});
    EXPECT_EQ(fill.exchange_timestamp, time_point{milliseconds{40}});
    EXPECT_EQ(fill.first_fill_event_time_delta_ms(), 10);
    EXPECT_EQ(fill.last_fill_event_time_delta_ms(), 10);
    EXPECT_EQ(fill.latency_ms(), 10);
}

TEST(ShadowTracker, InvalidFillDoesNotCreateOrMutateEconomicState)
{
    struct InvalidCase
    {
        fill_event fill;
        shadow_ingest_error expected;
    };

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    std::vector<InvalidCase> cases;
    cases.push_back(
        {make_fill(0, order_side::buy, 1.0, 100.0, 10), shadow_ingest_error::invalid_order_id});
    cases.push_back(
        {make_fill(1, order_side::buy, 1.0, 100.0, 10, ""), shadow_ingest_error::invalid_symbol});
    cases.push_back({make_fill(1, static_cast<order_side>(99), 1.0, 100.0, 10),
                     shadow_ingest_error::invalid_side});
    cases.push_back(
        {make_fill(1, order_side::buy, 0.0, 100.0, 10), shadow_ingest_error::invalid_quantity});
    cases.push_back(
        {make_fill(1, order_side::buy, -1.0, 100.0, 10), shadow_ingest_error::invalid_quantity});
    cases.push_back(
        {make_fill(1, order_side::buy, nan, 100.0, 10), shadow_ingest_error::invalid_quantity});
    cases.push_back({make_fill(1, order_side::buy, infinity, 100.0, 10),
                     shadow_ingest_error::invalid_quantity});
    cases.push_back(
        {make_fill(1, order_side::buy, 1.0, 0.0, 10), shadow_ingest_error::invalid_price});
    cases.push_back(
        {make_fill(1, order_side::buy, 1.0, -100.0, 10), shadow_ingest_error::invalid_price});
    cases.push_back(
        {make_fill(1, order_side::buy, 1.0, nan, 10), shadow_ingest_error::invalid_price});
    cases.push_back(
        {make_fill(1, order_side::buy, 1.0, infinity, 10), shadow_ingest_error::invalid_price});
    cases.push_back(
        {make_fill(1, order_side::buy, 1.0, 100.0, 0), shadow_ingest_error::invalid_timestamp});

    for (const auto& invalid : cases) {
        ShadowTracker tracker;
        const ShadowIngestResult result = tracker.try_on_simulated_fill(invalid.fill);
        EXPECT_FALSE(result);
        EXPECT_EQ(result.error, invalid.expected);
        EXPECT_FALSE(tracker.valid());
        EXPECT_EQ(tracker.error_count(), 1U);
        EXPECT_TRUE(tracker.fills().empty());
    }
}

TEST(ShadowTracker, IdentityMismatchRejectsWithoutMutatingExistingEntry)
{
    ShadowTracker tracker;
    ASSERT_TRUE(tracker.try_on_simulated_fill(make_fill(5, order_side::buy, 2.0, 100.0, 10)));
    const shadow_fill before = tracker.fills().at(5);

    const auto wrong_symbol =
        tracker.try_on_exchange_fill(make_fill(5, order_side::buy, 3.0, 101.0, 20, "ETHUSDT"));
    EXPECT_FALSE(wrong_symbol);
    EXPECT_EQ(wrong_symbol.error, shadow_ingest_error::identity_mismatch);

    const auto wrong_side =
        tracker.try_on_exchange_fill(make_fill(5, order_side::sell, 3.0, 101.0, 20));
    EXPECT_FALSE(wrong_side);
    EXPECT_EQ(wrong_side.error, shadow_ingest_error::identity_mismatch);

    const auto& after = tracker.fills().at(5);
    EXPECT_EQ(after.symbol, before.symbol);
    EXPECT_EQ(after.side, before.side);
    EXPECT_DOUBLE_EQ(after.sim_quantity, before.sim_quantity);
    EXPECT_DOUBLE_EQ(after.sim_price, before.sim_price);
    EXPECT_EQ(after.sim_slice_count, before.sim_slice_count);
    EXPECT_FALSE(after.exchange_filled);
    EXPECT_EQ(tracker.error_count(), 2U);
}

TEST(ShadowTracker, QuantityOverflowRejectsWithoutMutatingExistingLeg)
{
    ShadowTracker tracker;
    const double first_quantity = std::numeric_limits<double>::max() / 2.0;
    ASSERT_TRUE(
        tracker.try_on_simulated_fill(make_fill(6, order_side::buy, first_quantity, 100.0, 10)));
    const shadow_fill before = tracker.fills().at(6);

    const auto result = tracker.try_on_simulated_fill(
        make_fill(6, order_side::buy, std::numeric_limits<double>::max(), 200.0, 20));
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, shadow_ingest_error::quantity_overflow);
    const auto& after = tracker.fills().at(6);
    EXPECT_DOUBLE_EQ(after.sim_quantity, before.sim_quantity);
    EXPECT_DOUBLE_EQ(after.sim_price, before.sim_price);
    EXPECT_EQ(after.sim_slice_count, before.sim_slice_count);
}

TEST(ShadowTracker, SlippageBpsOverflowRejectsWithoutCommittingSecondLeg)
{
    ShadowTracker tracker;
    ASSERT_TRUE(tracker.try_on_simulated_fill(
        make_fill(11, order_side::buy, 1.0, std::numeric_limits<double>::min(), 10)));

    const auto result = tracker.try_on_exchange_fill(
        make_fill(11, order_side::buy, 1.0, std::numeric_limits<double>::max(), 20));

    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, shadow_ingest_error::arithmetic_overflow);
    const auto& after = tracker.fills().at(11);
    EXPECT_TRUE(after.sim_filled);
    EXPECT_FALSE(after.exchange_filled);
    EXPECT_DOUBLE_EQ(after.exchange_quantity, 0.0);
    EXPECT_DOUBLE_EQ(after.exchange_price, 0.0);
}

TEST(ShadowTracker, OnlineVwapAvoidsPriceQuantityProductOverflow)
{
    ShadowTracker tracker;
    ASSERT_TRUE(tracker.try_on_simulated_fill(make_fill(8, order_side::buy, 1.0e200, 1.0e200, 10)));
    ASSERT_TRUE(tracker.try_on_simulated_fill(make_fill(8, order_side::buy, 1.0e200, 2.0e200, 20)));

    const auto& fill = tracker.fills().at(8);
    EXPECT_TRUE(std::isfinite(fill.sim_quantity));
    EXPECT_TRUE(std::isfinite(fill.sim_price));
    EXPECT_DOUBLE_EQ(fill.sim_quantity, 2.0e200);
    EXPECT_DOUBLE_EQ(fill.sim_price, 1.5e200);
}

TEST(ShadowTracker, SliceSplittingPreservesEconomicVwap)
{
    ShadowTracker single;
    ASSERT_TRUE(single.try_on_simulated_fill(make_fill(9, order_side::buy, 10.0, 123.5, 10)));

    ShadowTracker split;
    for (std::int64_t index = 0; index < 10; ++index) {
        ASSERT_TRUE(
            split.try_on_simulated_fill(make_fill(9, order_side::buy, 1.0, 123.5, 10 + index)));
    }

    EXPECT_DOUBLE_EQ(split.fills().at(9).sim_quantity, single.fills().at(9).sim_quantity);
    EXPECT_DOUBLE_EQ(split.fills().at(9).sim_price, single.fills().at(9).sim_price);
}

TEST(ShadowTracker, SeededSlicePermutationsMatchIndependentLongDoubleOracle)
{
    struct Slice
    {
        bool simulated;
        double quantity;
        double price;
        std::int64_t timestamp_ms;
    };

    std::mt19937_64 generator(0x5A17D0ULL);
    std::uniform_real_distribution<double> quantity_distribution(0.001, 50.0);
    std::uniform_real_distribution<double> price_distribution(0.01, 100000.0);

    for (std::size_t trial = 0; trial < 200; ++trial) {
        std::vector<Slice> slices;
        long double simulated_quantity = 0.0L;
        long double simulated_notional = 0.0L;
        long double exchange_quantity = 0.0L;
        long double exchange_notional = 0.0L;

        for (std::size_t index = 0; index < 32; ++index) {
            const bool simulated = index % 2 == 0;
            const double quantity = quantity_distribution(generator);
            const double price = price_distribution(generator);
            slices.push_back({simulated, quantity, price, static_cast<std::int64_t>(index + 1)});
            if (simulated) {
                simulated_quantity += static_cast<long double>(quantity);
                simulated_notional +=
                    static_cast<long double>(quantity) * static_cast<long double>(price);
            } else {
                exchange_quantity += static_cast<long double>(quantity);
                exchange_notional +=
                    static_cast<long double>(quantity) * static_cast<long double>(price);
            }
        }
        std::shuffle(slices.begin(), slices.end(), generator);

        ShadowTracker tracker;
        for (const auto& input : slices) {
            const fill_event fill =
                make_fill(10, order_side::buy, input.quantity, input.price, input.timestamp_ms);
            const auto result = input.simulated ? tracker.try_on_simulated_fill(fill)
                                                : tracker.try_on_exchange_fill(fill);
            ASSERT_TRUE(result);
        }

        const auto& actual = tracker.fills().at(10);
        const double expected_simulated_vwap =
            static_cast<double>(simulated_notional / simulated_quantity);
        const double expected_exchange_vwap =
            static_cast<double>(exchange_notional / exchange_quantity);
        EXPECT_NEAR(actual.sim_quantity, static_cast<double>(simulated_quantity),
                    static_cast<double>(simulated_quantity) * 2.0e-15);
        EXPECT_NEAR(actual.exchange_quantity, static_cast<double>(exchange_quantity),
                    static_cast<double>(exchange_quantity) * 2.0e-15);
        EXPECT_NEAR(actual.sim_price, expected_simulated_vwap, expected_simulated_vwap * 2.0e-15);
        EXPECT_NEAR(actual.exchange_price, expected_exchange_vwap,
                    expected_exchange_vwap * 2.0e-15);
    }
}

TEST(ShadowTracker, ExistingOrderAccumulationDoesNotAllocate)
{
    ShadowTracker tracker;
    const fill_event fill = make_fill(12, order_side::buy, 0.001, 123.5, 10);
    ASSERT_TRUE(tracker.try_on_simulated_fill(fill));

    bool all_accepted = true;
    truetest::test::alloc::measure_window measured;
    for (std::size_t index = 0; index < 100000; ++index)
        all_accepted = static_cast<bool>(tracker.try_on_simulated_fill(fill)) && all_accepted;
    const auto allocations = measured.total();

    EXPECT_TRUE(all_accepted);
    EXPECT_EQ(allocations.count, 0U);
    EXPECT_EQ(allocations.bytes, 0U);
}

TEST(ShadowTracker, ReportLabelsConditionalObservedEvidenceAndRestoresStreamState)
{
    ShadowTracker tracker;
    tracker.on_simulated_fill(make_fill(14, order_side::buy, 1.0, 100.0, 10));

    std::ostringstream output;
    const auto previous_flags = std::cout.flags();
    const auto previous_precision = std::cout.precision();
    std::cout << std::scientific << std::setprecision(3);
    const auto expected_flags = std::cout.flags();
    const auto expected_precision = std::cout.precision();
    auto* original_buffer = std::cout.rdbuf(output.rdbuf());
    tracker.print_report();
    std::cout.rdbuf(original_buffer);

    const auto actual_flags = std::cout.flags();
    const auto actual_precision = std::cout.precision();
    std::cout.flags(previous_flags);
    std::cout.precision(previous_precision);

    EXPECT_NE(output.str().find("Observed order IDs"), std::string::npos);
    EXPECT_NE(output.str().find("Sim leg presence (observed IDs)"), std::string::npos);
    EXPECT_EQ(output.str().find("fill rate"), std::string::npos);
    EXPECT_EQ(actual_flags, expected_flags);
    EXPECT_EQ(actual_precision, expected_precision);
}

TEST(ShadowTracker, ResetClearsEvidenceAndErrorLatch)
{
    ShadowTracker tracker;
    EXPECT_FALSE(tracker.try_on_simulated_fill(make_fill(0, order_side::buy, 1.0, 100.0, 10)));
    EXPECT_FALSE(tracker.valid());

    tracker.reset();

    EXPECT_TRUE(tracker.valid());
    EXPECT_EQ(tracker.error_count(), 0U);
    EXPECT_EQ(tracker.last_error(), shadow_ingest_error::none);
    EXPECT_TRUE(tracker.fills().empty());
}
