#include <gtest/gtest.h>
#include "analytics/bar_aggregator.h"

static auto epoch_ms(int64_t ms)
{
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

TEST(BarAggregator, SingleTick_NoEmit)
{
    int count = 0;
    BarAggregator agg(std::chrono::milliseconds(100), [&](const market_event&) { count++; });
    agg.on_tick("X", 100.0, 10, epoch_ms(0));
    EXPECT_EQ(count, 0);
}

TEST(BarAggregator, IntervalComplete_EmitsBar)
{
    int count = 0;
    market_event last_bar(epoch_ms(0), "", 0, 0, 0, 0);

    BarAggregator agg(std::chrono::milliseconds(100), [&](const market_event& bar) {
        count++;
        last_bar = bar;
    });

    agg.on_tick("X", 100.0, 10, epoch_ms(0));
    agg.on_tick("X", 110.0, 20, epoch_ms(50));
    agg.on_tick("X", 105.0, 30, epoch_ms(100)); // triggers new bar, emits previous

    EXPECT_EQ(count, 1);
    EXPECT_DOUBLE_EQ(last_bar.get_open(), 100.0);
    EXPECT_DOUBLE_EQ(last_bar.get_high(), 110.0);
    EXPECT_DOUBLE_EQ(last_bar.get_low(), 100.0);
    EXPECT_DOUBLE_EQ(last_bar.get_close(), 110.0);
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
    agg.flush();
    EXPECT_EQ(count, 1);
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
    // Note: emit_bar() sets bar_open_=false, so the next tick after an emit
    // re-opens the bar (enters the !bar_open_ branch). Each bar needs at least
    // 2 ticks: one to open, one to trigger the next emit.
    int count = 0;
    BarAggregator agg(std::chrono::milliseconds(100), [&](const market_event&) { count++; });

    // Bar 1: t=0 opens, t=101 emits
    agg.on_tick("X", 1.0, 1, epoch_ms(0));
    agg.on_tick("X", 1.0, 1, epoch_ms(101));  // emits bar 1, bar_open_=false

    // Bar 2: t=102 re-opens, t=203 emits
    agg.on_tick("X", 1.0, 1, epoch_ms(102));
    agg.on_tick("X", 1.0, 1, epoch_ms(203));  // emits bar 2, bar_open_=false

    // Bar 3: t=204 re-opens, t=305 emits
    agg.on_tick("X", 1.0, 1, epoch_ms(204));
    agg.on_tick("X", 1.0, 1, epoch_ms(305));  // emits bar 3

    EXPECT_EQ(count, 3);
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
