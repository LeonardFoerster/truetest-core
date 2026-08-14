#include <gtest/gtest.h>

#include "ui/console_format.h"
#include "ui/console_dashboard.h"   // for enums

TEST(ConsoleFormat, FmtU64)
{
    EXPECT_EQ(truetest::ui::fmt_u64(0), "0");
    EXPECT_EQ(truetest::ui::fmt_u64(123), "123");
    EXPECT_EQ(truetest::ui::fmt_u64(1000), "1,000");
    EXPECT_EQ(truetest::ui::fmt_u64(1234567), "1,234,567");
}

TEST(ConsoleFormat, FmtPriceFp8)
{
    EXPECT_EQ(truetest::ui::fmt_price_fp8(-1), "-");
    EXPECT_EQ(truetest::ui::fmt_price_fp8(100000000), "1.00");          // 1.0
    EXPECT_EQ(truetest::ui::fmt_price_fp8(123456789), "1.23");          // ~1.23456789
}

TEST(ConsoleFormat, FmtPnlFp4)
{
    EXPECT_EQ(truetest::ui::fmt_pnl_fp4(0), "+$0.00");
    EXPECT_EQ(truetest::ui::fmt_pnl_fp4(12345), "+$1.23");
    EXPECT_EQ(truetest::ui::fmt_pnl_fp4(-10000), "-$1.00");
}

TEST(ConsoleFormat, FmtToxicity)
{
    EXPECT_EQ(truetest::ui::fmt_toxicity_bps_fp2(123, 0), "-");
    EXPECT_EQ(truetest::ui::fmt_toxicity_bps_fp2(123, 5), "+1.23 bps");
}

TEST(ConsoleFormat, FmtPosition)
{
    EXPECT_EQ(truetest::ui::fmt_position_fp8(0), "flat");
    EXPECT_EQ(truetest::ui::fmt_position_fp8(100000000), "long 1.0000");
    EXPECT_EQ(truetest::ui::fmt_position_fp8(-25000000), "short 0.2500");
}

TEST(ConsoleFormat, VisibleWidthAndPad)
{
    using namespace truetest::ui;
    EXPECT_EQ(visible_width_utf8("hello"), 5);
    EXPECT_EQ(visible_width_utf8("\x1b[31mred\x1b[0m"), 3);   // ANSI ignored
    EXPECT_EQ(pad_right("hi", 5), "hi   ");
}

TEST(ConsoleFormat, RowProducesBox)
{
    std::string r = truetest::ui::row("content", true);
    EXPECT_TRUE(r.find("│") != std::string::npos);
    EXPECT_TRUE(r.find("content") != std::string::npos);
}

// spread_bps()/total_ring_drops() were previously hand-computed inline,
// separately, in both console_dashboard.cpp and tabbed_dashboard.cpp -
// factored out during the TUI dedup pass (see AGENTS.md's dashboard_snapshot
// note). Covered here since they're now the single source of truth for both.
TEST(ConsoleFormat, SpreadBps)
{
    using truetest::ui::spread_bps;
    EXPECT_DOUBLE_EQ(spread_bps(100.0, 100.0), 0.0);
    // (101-99)/100 * 1e4 = 200 bps
    EXPECT_DOUBLE_EQ(spread_bps(99.0, 101.0), 200.0);
    EXPECT_DOUBLE_EQ(spread_bps(0.0, 100.0), 0.0);   // non-positive bid -> 0
    EXPECT_DOUBLE_EQ(spread_bps(100.0, -1.0), 0.0);  // non-positive ask -> 0
}

TEST(ConsoleFormat, TotalRingDropsSumsAllSixCounters)
{
    using truetest::ui::streaming_stats;
    using truetest::ui::total_ring_drops;
    streaming_stats s;
    EXPECT_EQ(total_ring_drops(s), 0u);

    s.ring_drops_logging.store(1);
    s.ring_drops_risk.store(2);
    s.ring_drops_stats.store(3);
    s.ring_drops_observer.store(4);
    s.ring_drops_risk_stats.store(5);
    s.ring_drops_mm.store(6);
    EXPECT_EQ(total_ring_drops(s), 21u);
}
