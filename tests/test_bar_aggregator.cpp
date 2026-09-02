#include <gtest/gtest.h>
#include "analytics/bar_aggregator.h"

static auto epoch_ms(int64_t ms)
{
    // Keep ordinary fixtures away from the explicitly invalid epoch-zero
    // sentinel while preserving every relative interval in the tests.
    return std::chrono::system_clock::time_point(
        std::chrono::milliseconds(ms + 1));
}

TEST(BarAggregator, SingleTick_DoesNotEmitPartial)
{
    int count = 0;
    BarAggregator agg(std::chrono::milliseconds(100), [&](const market_event&) { count++; });
    agg.on_tick("X", 100.0, 10, epoch_ms(0));
    // No partial emission — a bar is only emitted once its interval completes
    // (or via flush). Partial emissions would double-feed strategies/indicators.
    EXPECT_EQ(count, 0);
}

TEST(BarAggregator, NoEmissionsWithinInterval_Deterministic)
{
    int count = 0;
    BarAggregator agg(std::chrono::milliseconds(1000), [&](const market_event&) { count++; });
    // Many ticks inside one interval: exactly zero emissions until rollover,
    // independent of wall-clock time / host speed.
    for (int i = 0; i < 100; ++i)
        agg.on_tick("X", 100.0 + i, 1, epoch_ms(i));
    EXPECT_EQ(count, 0);
    agg.on_tick("X", 50.0, 1, epoch_ms(1000)); // rollover → exactly one bar
    EXPECT_EQ(count, 1);
    agg.flush();                               // final partial
    EXPECT_EQ(count, 2);
}

TEST(BarAggregator, IntervalComplete_EmitsBar)
{
    market_event last_bar(epoch_ms(0), "", 0, 0, 0, 0);
    std::vector<market_event> emitted;

    BarAggregator agg(std::chrono::milliseconds(100), [&](const market_event& bar) {
        emitted.push_back(bar);
        last_bar = bar;
    });

    agg.on_tick("X", 100.0, 10, epoch_ms(0));
    agg.on_tick("X", 110.0, 20, epoch_ms(50));
    agg.on_tick("X", 105.0, 30, epoch_ms(100)); // triggers new bar

    // Final bar (from the completed interval) should have correct OHLC
    // Find the bar with open=100 (the completed bar, not the new bar starting at t=100)
    bool found_completed = false;
    for (auto& b : emitted) {
        if (b.get_open() == 100.0 && b.get_close() == 110.0) {
            found_completed = true;
            EXPECT_DOUBLE_EQ(b.get_high(), 110.0);
            EXPECT_DOUBLE_EQ(b.get_low(), 100.0);
        }
    }
    EXPECT_TRUE(found_completed);
}

TEST(BarAggregator, OHLC_Correctness)
{
    market_event last_bar(epoch_ms(0), "", 0, 0, 0, 0);
    BarAggregator agg(std::chrono::milliseconds(1000), [&](const market_event& bar) {
        last_bar = bar;
    });

    agg.on_tick("A", 100.0, 1, epoch_ms(0));
    agg.on_tick("A", 110.0, 1, epoch_ms(10));
    agg.on_tick("A", 90.0,  1, epoch_ms(20));
    agg.on_tick("A", 105.0, 1, epoch_ms(30));
    agg.flush();

    EXPECT_DOUBLE_EQ(last_bar.get_open(), 100.0);
    EXPECT_DOUBLE_EQ(last_bar.get_high(), 110.0);
    EXPECT_DOUBLE_EQ(last_bar.get_low(), 90.0);
    EXPECT_DOUBLE_EQ(last_bar.get_close(), 105.0);
}

TEST(BarAggregator, Volume_Accumulation)
{
    market_event last_bar(epoch_ms(0), "", 0, 0, 0, 0);
    BarAggregator agg(std::chrono::milliseconds(1000), [&](const market_event& bar) {
        last_bar = bar;
    });

    agg.on_tick("A", 1.0, 10, epoch_ms(0));
    agg.on_tick("A", 1.0, 20, epoch_ms(10));
    agg.on_tick("A", 1.0, 30, epoch_ms(20));
    agg.flush();

    EXPECT_EQ(last_bar.get_volume(), 60);
}

TEST(BarAggregator, Flush_EmitsPartialBar)
{
    int count = 0;
    BarAggregator agg(std::chrono::milliseconds(10000), [&](const market_event&) { count++; });
    agg.on_tick("X", 1.0, 1, epoch_ms(0));
    EXPECT_EQ(count, 0); // nothing emitted while the bar is still open
    agg.flush();
    EXPECT_EQ(count, 1); // flush emits the final partial bar
    agg.flush();
    EXPECT_EQ(count, 1); // an already-flushed partial is not duplicated
}

TEST(BarAggregator, Flush_NothingOpen)
{
    int count = 0;
    BarAggregator agg(std::chrono::milliseconds(100), [&](const market_event&) { count++; });
    agg.flush();
    EXPECT_EQ(count, 0);
}

TEST(BarAggregator, MultipleBarEmissions)
{
    // Track unique bar start times (completed bars)
    std::set<int64_t> bar_opens;
    BarAggregator agg(std::chrono::milliseconds(100), [&](const market_event& bar) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            bar.get_timestamp().time_since_epoch()).count();
        bar_opens.insert(ms);
    });

    // Bar 1: t=0
    agg.on_tick("X", 1.0, 1, epoch_ms(0));
    // Bar 2: t=101 triggers bar 1 completion, starts new bar
    agg.on_tick("X", 1.0, 1, epoch_ms(101));
    // Bar 3: t=203 triggers bar 2 completion, starts new bar
    agg.on_tick("X", 1.0, 1, epoch_ms(203));
    // Bar 4: t=305 triggers bar 3 completion, starts new bar
    agg.on_tick("X", 1.0, 1, epoch_ms(305));
    // The last open bar is only emitted on flush.
    agg.flush();

    // 4 distinct bar start times
    EXPECT_EQ(bar_opens.size(), 4u);
}

TEST(BarAggregator, Symbol_Propagation)
{
    std::string sym;
    BarAggregator agg(std::chrono::milliseconds(1000), [&](const market_event& bar) {
        sym = bar.get_symbol();
    });
    agg.on_tick("BTC", 1.0, 1, epoch_ms(0));
    agg.flush();
    EXPECT_EQ(sym, "BTC");
}

TEST(BarAggregator, InterleavedSymbolsKeepIndependentBars)
{
    std::vector<market_event> bars;
    BarAggregator agg(std::chrono::milliseconds(1000),
                      [&](const market_event& bar) { bars.push_back(bar); });

    EXPECT_TRUE(agg.on_tick("BTC", 100.0, 100, epoch_ms(0), 1));
    EXPECT_TRUE(agg.on_tick("ETH", 1000.0, 200'000'000,
                            epoch_ms(10), 100'000'000ULL));
    EXPECT_TRUE(agg.on_tick("BTC", 110.0, 50, epoch_ms(20), 1));
    EXPECT_TRUE(agg.on_tick("ETH", 900.0, 100'000'000,
                            epoch_ms(30), 100'000'000ULL));
    agg.flush();

    ASSERT_EQ(bars.size(), 2u);
    EXPECT_EQ(bars[0].get_symbol(), "BTC");
    EXPECT_DOUBLE_EQ(bars[0].get_open(), 100.0);
    EXPECT_DOUBLE_EQ(bars[0].get_close(), 110.0);
    EXPECT_EQ(bars[0].get_volume(), 150);
    EXPECT_EQ(bars[0].get_quantity_scale(), 1u);
    EXPECT_EQ(bars[1].get_symbol(), "ETH");
    EXPECT_DOUBLE_EQ(bars[1].get_open(), 1000.0);
    EXPECT_DOUBLE_EQ(bars[1].get_close(), 900.0);
    EXPECT_EQ(bars[1].get_volume(), 300'000'000);
    EXPECT_EQ(bars[1].get_quantity_scale(), 100'000'000ULL);
}

TEST(BarAggregator, FlushEmitsOpenSymbolsInEventTimeOrder)
{
    std::vector<market_event> bars;
    BarAggregator agg(std::chrono::milliseconds(1000),
                      [&](const market_event& bar) { bars.push_back(bar); });

    ASSERT_TRUE(agg.on_tick("A", 100.0, 1, epoch_ms(0)));
    ASSERT_TRUE(agg.on_tick("B", 200.0, 1, epoch_ms(10)));
    // Rolls A and emits its old bar; A's new partial is newer than B's.
    ASSERT_TRUE(agg.on_tick("A", 101.0, 1, epoch_ms(2000)));
    agg.flush();

    ASSERT_EQ(bars.size(), 3u);
    EXPECT_EQ(bars[0].get_symbol(), "A");
    EXPECT_EQ(bars[0].get_timestamp(), epoch_ms(0));
    EXPECT_EQ(bars[1].get_symbol(), "B");
    EXPECT_EQ(bars[1].get_timestamp(), epoch_ms(10));
    EXPECT_EQ(bars[2].get_symbol(), "A");
    EXPECT_EQ(bars[2].get_timestamp(), epoch_ms(2000));
}

TEST(BarAggregator, SymbolCapacityFailsClosedWithoutMixing)
{
    int emitted = 0;
    BarAggregator agg(std::chrono::milliseconds(1000),
                      [&](const market_event&) { ++emitted; }, 1);
    EXPECT_TRUE(agg.on_tick("A", 1.0, 1, epoch_ms(0)));
    EXPECT_FALSE(agg.on_tick("B", 2.0, 1, epoch_ms(2000)));
    EXPECT_EQ(emitted, 0) << "a rejected tick must not publish due bars";
    EXPECT_TRUE(agg.on_tick("A", 1.5, 1, epoch_ms(500)))
        << "a rejected tick must not advance the global time watermark";
    agg.flush();
    EXPECT_EQ(emitted, 1);
}

TEST(BarAggregator, InvalidTickFailsWithoutMutatingOpenBar)
{
    std::vector<market_event> bars;
    BarAggregator agg(std::chrono::milliseconds(1000),
                      [&](const market_event& bar) { bars.push_back(bar); });
    ASSERT_TRUE(agg.on_tick("A", 100.0, 5, epoch_ms(0)));
    EXPECT_FALSE(agg.on_tick(
        "A", 90.0, 1, std::chrono::system_clock::time_point{}));
    EXPECT_FALSE(agg.on_tick("A", 90.0, 1, epoch_ms(-1)));
    EXPECT_FALSE(agg.on_tick("A", 200.0, -1, epoch_ms(1)));
    EXPECT_FALSE(agg.on_tick("A", 200.0, 0, epoch_ms(1)));
    EXPECT_FALSE(agg.on_tick("A", std::numeric_limits<double>::infinity(),
                             1, epoch_ms(2)));
    agg.flush();
    ASSERT_EQ(bars.size(), 1u);
    EXPECT_DOUBLE_EQ(bars[0].get_close(), 100.0);
    EXPECT_EQ(bars[0].get_volume(), 5);
}

TEST(BarAggregator, RejectsGlobalAndPerSymbolTimeRegression)
{
    std::vector<market_event> bars;
    BarAggregator agg(std::chrono::milliseconds(1000),
                      [&](const market_event& bar) { bars.push_back(bar); });
    ASSERT_TRUE(agg.on_tick("A", 100.0, 1, epoch_ms(2000)));
    EXPECT_FALSE(agg.on_tick("A", 90.0, 1, epoch_ms(1000)));
    EXPECT_FALSE(agg.on_tick("B", 90.0, 1, epoch_ms(1500)));
    agg.flush();
    ASSERT_EQ(bars.size(), 1u);
    EXPECT_DOUBLE_EQ(bars[0].get_close(), 100.0);
}

TEST(BarAggregator, MixedQuantityScalesNeverRoundEconomicVolume)
{
    std::vector<market_event> bars;
    BarAggregator agg(std::chrono::milliseconds(1000),
                      [&](const market_event& bar) { bars.push_back(bar); });
    ASSERT_TRUE(agg.on_tick("A", 100.0, 1, epoch_ms(0), 2));
    EXPECT_FALSE(agg.on_tick("A", 200.0, 1, epoch_ms(1), 3));
    EXPECT_TRUE(agg.on_tick("A", 110.0, 3, epoch_ms(2), 6));
    agg.flush();

    ASSERT_EQ(bars.size(), 1u);
    EXPECT_DOUBLE_EQ(bars[0].get_close(), 110.0);
    EXPECT_EQ(bars[0].get_volume(), 2);
    EXPECT_EQ(bars[0].get_quantity_scale(), 2u);
}

TEST(BarAggregator, QuantityScaleAboveDoubleIntegerRangeRemainsExact)
{
    constexpr std::uint64_t large_scale = 9'007'199'254'740'993ULL;
    std::vector<market_event> bars;
    BarAggregator agg(std::chrono::milliseconds(1000),
                      [&](const market_event& bar) { bars.push_back(bar); });
    ASSERT_TRUE(agg.on_tick("A", 100.0, 1, epoch_ms(0), large_scale));
    ASSERT_TRUE(agg.on_tick("A", 101.0, 1, epoch_ms(1), 1));
    agg.flush();

    ASSERT_EQ(bars.size(), 1u);
    EXPECT_EQ(bars.front().get_quantity_scale(), large_scale);
    EXPECT_EQ(bars.front().get_volume(), 9'007'199'254'740'994LL);
}
