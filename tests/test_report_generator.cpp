#include "analytics/ascii_widgets.h"
#include "analytics/report_generator.h"

#include <gtest/gtest.h>

using tt::ascii::display_width;
using tt::ascii::equal_width_bins;
using tt::ascii::fmt_money;
using tt::ascii::fmt_signed_pct;
using tt::ascii::hbar;
using tt::ascii::section_header;
using tt::ascii::sparkline;
using tt::ascii::table;

TEST(AsciiWidgets, DisplayWidthCountsCodepoints)
{
    EXPECT_EQ(display_width("abc"), 3u);
    EXPECT_EQ(display_width("━━━"), 3u);       // each ━ is 3 bytes, 1 codepoint
    EXPECT_EQ(display_width("a━b"), 3u);
}

TEST(AsciiWidgets, HbarWidthInCodepoints)
{
    // Empty bar: all ░, but still `width` codepoints.
    EXPECT_EQ(display_width(hbar(0.0, 1.0, 10)), 10u);
    // Full bar.
    EXPECT_EQ(display_width(hbar(1.0, 1.0, 10)), 10u);
    // Half bar.
    EXPECT_EQ(display_width(hbar(0.5, 1.0, 10)), 10u);
    // Guards against div-by-zero.
    EXPECT_EQ(display_width(hbar(1.0, 0.0, 5)), 5u);
}

TEST(AsciiWidgets, SectionHeaderPadsToWidth)
{
    std::string h = section_header("Returns", 40);
    EXPECT_EQ(display_width(h), 40u);
}

TEST(AsciiWidgets, SparklineLengthMatchesInputOrMaxWidth)
{
    std::vector<double> small{1, 2, 3, 4, 5};
    EXPECT_EQ(display_width(sparkline(small, 20)), 5u);

    std::vector<double> big(200, 0.0);
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<double>(i);
    EXPECT_EQ(display_width(sparkline(big, 40)), 40u);
}

TEST(AsciiWidgets, EqualWidthBinsCountAll)
{
    std::vector<double> v{-2, -1, 0, 0, 1, 2, 2, 3};
    auto bins = equal_width_bins(v, 5);
    EXPECT_EQ(bins.size(), 5u);

    double total = 0.0;
    for (const auto& b : bins) total += b.value;
    EXPECT_DOUBLE_EQ(total, static_cast<double>(v.size()));
}

TEST(AsciiWidgets, FormatHelpers)
{
    EXPECT_EQ(fmt_signed_pct(0.1842), "+18.42%");
    EXPECT_EQ(fmt_signed_pct(-0.05), "-5.00%");
    EXPECT_EQ(fmt_money(1234567.89), "1,234,567.89");
    EXPECT_EQ(fmt_money(-1234.5), "-1,234.50");
    EXPECT_EQ(fmt_money(0.0), "0.00");
}

TEST(AsciiWidgets, TableAlignsColumns)
{
    std::string t = table(
        {"name", "pnl"},
        {{"aaa", "1.00"}, {"b", "-200.50"}},
        {tt::ascii::align::left, tt::ascii::align::right});
    // Separator line length = header1_width + 2 + header2_width.
    // Largest cell widths: "name"=4 and "-200.50"=7. Total = 4+2+7 = 13.
    EXPECT_NE(t.find("----  -------"), std::string::npos);
}

TEST(ReportGenerator, RendersBasicSections)
{
    AnalyticsReport r;
    r.initial_equity = 100000.0;
    r.final_equity = 118420.0;
    r.cumulative_return = 0.1842;
    r.buy_and_hold_return = 0.10;
    r.sharpe_ratio = 1.84;
    r.sortino_ratio = 2.41;
    r.max_drawdown = 12.3;
    r.total_trades = 127;
    r.win_rate = 58.3;
    r.profit_factor = 1.73;
    r.trade_returns = {-2.1, -1.0, -0.5, 0.0, 0.2, 0.5, 1.0, 1.2, 1.8, 2.4};
    r.equity_curve = {
        {std::chrono::system_clock::now(), 100000.0},
        {std::chrono::system_clock::now(), 101000.0},
        {std::chrono::system_clock::now(), 118420.0},
    };

    std::string out = tt::render_report(r);

    EXPECT_NE(out.find("Analytics Report"),            std::string::npos);
    EXPECT_NE(out.find("Returns"),                     std::string::npos);
    EXPECT_NE(out.find("+18.42%"),                     std::string::npos);
    EXPECT_NE(out.find("Risk"),                        std::string::npos);
    EXPECT_NE(out.find("Trades"),                      std::string::npos);
    EXPECT_NE(out.find("Per-Trade PnL Distribution"),  std::string::npos);
    EXPECT_NE(out.find("Equity Curve"),                std::string::npos);
}

TEST(ReportGenerator, HonoursSectionToggles)
{
    AnalyticsReport r;
    r.total_trades = 1;

    tt::report_options o;
    o.include_returns = false;
    o.include_risk = false;
    o.include_trades = true;
    o.include_execution = false;
    o.include_exposure = false;
    o.include_distribution = false;
    o.include_equity_sparkline = false;
    o.include_benchmark = false;
    o.include_per_symbol = false;
    o.include_per_strategy = false;
    o.include_worst_trades = false;

    std::string out = tt::render_report(r, o);
    EXPECT_EQ(out.find("Returns"),  std::string::npos);
    EXPECT_EQ(out.find("Risk"),     std::string::npos);
    EXPECT_NE(out.find("Trades"),   std::string::npos);
}

TEST(ReportGenerator, WorstTradesSortedByPnlAscending)
{
    AnalyticsReport r;
    auto now = std::chrono::system_clock::now();
    trade_record a{1, order_side::buy,  1.0, 10.0, 0.0, 10.0, now, -50.0, "BTC", "sma"};
    trade_record b{2, order_side::sell, 1.0, 12.0, 0.0, 12.0, now, +30.0, "BTC", "sma"};
    trade_record c{3, order_side::sell, 1.0, 11.0, 0.0, 11.0, now, -10.0, "BTC", "sma"};
    r.trades = {a, b, c};

    std::string out = tt::render_worst_trades_section(r, {});
    auto p_minus50 = out.find("-50.00");
    auto p_minus10 = out.find("-10.00");
    ASSERT_NE(p_minus50, std::string::npos);
    ASSERT_NE(p_minus10, std::string::npos);
    EXPECT_LT(p_minus50, p_minus10);
}
