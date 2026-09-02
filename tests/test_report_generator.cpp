#include "analytics/ascii_widgets.h"
#include "analytics/report_generator.h"
#include "web/json_emit.h"
#include "web/report_json.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <charconv>
#include <clocale>
#include <limits>

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

TEST(ReportJsonNumber, FiniteDoublesRoundTripExactly)
{
    const std::array values{
        0.1,
        1'000'000'000'000'001.0,
        -1'000'000'000'000'001.0,
        std::numeric_limits<double>::denorm_min(),
        std::numeric_limits<double>::min(),
        std::numeric_limits<double>::max(),
    };
    for (const double value : values)
    {
        SCOPED_TRACE(value);
        std::string json;
        truetest::web::jx::Json(json).arr().num(value).endarr();
        ASSERT_GE(json.size(), 3u);
        double parsed = 0.0;
        const auto [end, error] = std::from_chars(
            json.data() + 1, json.data() + json.size() - 1, parsed,
            std::chars_format::general);
        ASSERT_EQ(error, std::errc{});
        EXPECT_EQ(end, json.data() + json.size() - 1);
        EXPECT_DOUBLE_EQ(parsed, value);
    }
}

TEST(ReportJsonNumber, EncodingIsIndependentOfNumericLocale)
{
    const char* current = std::setlocale(LC_NUMERIC, nullptr);
    const std::string original = current ? current : "C";
    const char* changed = std::setlocale(LC_NUMERIC, "de_DE.utf8");
    if (!changed)
    {
        std::setlocale(LC_NUMERIC, original.c_str());
        GTEST_SKIP() << "de_DE.utf8 locale unavailable";
    }

    std::string json;
    truetest::web::jx::Json(json).obj().kv("value", 1.5).endobj();
    std::setlocale(LC_NUMERIC, original.c_str());
    EXPECT_EQ(json, R"({"value":1.5})");
}

TEST(ReportJsonNumber, NonFiniteDoublesFailClosed)
{
    for (const double value : {
             std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::infinity(),
             -std::numeric_limits<double>::infinity()})
    {
        std::string json;
        EXPECT_THROW(truetest::web::jx::Json(json).arr().num(value),
                     std::invalid_argument);
    }
}

TEST(ReportJsonString, InvalidUtf8FailsClosed)
{
    for (const std::string& invalid : {
             std::string(1, static_cast<char>(0xff)),
             std::string("\xc0\x80", 2),
             std::string("\xed\xa0\x80", 3),
             std::string("\xf4\x90\x80\x80", 4),
             std::string("\xe2\x82", 2),
         })
    {
        std::string json;
        EXPECT_THROW(truetest::web::jx::Json(json).str(invalid),
                     std::invalid_argument);
    }
}

TEST(ReportJsonString, ValidUtf8RoundTripsUnchanged)
{
    const std::string value = "BTC-€-日本";
    std::string json;
    truetest::web::jx::Json(json).str(value);
    EXPECT_EQ(nlohmann::json::parse(json).get<std::string>(), value);
}

TEST(ReportJson, UnboundedProfitFactorUsesExplicitStatusWithoutSentinel)
{
    AnalyticsReport report;
    report.initial_equity = 1'000.0;
    report.final_equity = 1'010.0;
    report.total_win = 10.0;
    report.profit_factor = 0.0;
    report.profit_factor_unbounded = true;
    report.profit_factor_reason = "no_losses_unbounded";
    sub_analytics symbol;
    symbol.total_pnl = 10.0;
    symbol.trade_count = 1;
    symbol.win_count = 1;
    symbol.total_win = 10.0;
    report.per_symbol.emplace("X", symbol);

    const auto parsed = nlohmann::json::parse(report.to_results_json());
    EXPECT_DOUBLE_EQ(parsed.at("profit_factor").get<double>(), 0.0);
    EXPECT_FALSE(parsed.at("profit_factor_valid").get<bool>());
    EXPECT_TRUE(parsed.at("profit_factor_unbounded").get<bool>());
    EXPECT_EQ(parsed.at("profit_factor_reason"), "no_losses_unbounded");
    const auto& sub = parsed.at("per_symbol").at("X");
    EXPECT_DOUBLE_EQ(sub.at("profit_factor").get<double>(), 0.0);
    EXPECT_TRUE(sub.at("profit_factor_unbounded").get<bool>());
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
    r.gross_realized_pnl = 20'000.0;
    r.realized_pnl = 18'420.0;
    r.total_commission = 1'580.0;
    r.reconciliation_residual = 0.0;
    r.total_trades = 127;
    r.win_rate = 58.3;
    r.total_win = 1'730.0;
    r.total_loss = 1'000.0;
    r.profit_factor = 1.73;
    r.profit_factor_valid = true;
    r.profit_factor_reason = "computed_from_gross_win_and_loss";
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
    EXPECT_NE(out.find("realized gross"),              std::string::npos);
    EXPECT_NE(out.find("reconciliation residual"),     std::string::npos);
    EXPECT_NE(out.find("Risk"),                        std::string::npos);
    EXPECT_NE(out.find("Trades"),                      std::string::npos);
    EXPECT_NE(out.find("Closing-Fill PnL Distribution"), std::string::npos);
    EXPECT_NE(out.find("Equity Curve"),                std::string::npos);
}

TEST(ReportGenerator, LabelsSyntheticExecutionAsExploratory)
{
    AnalyticsReport r;
    r.contains_exploratory_execution = true;

    const std::string out = tt::render_execution_section(r, {});
    EXPECT_NE(out.find("EXPLORATORY"), std::string::npos);
    EXPECT_NE(out.find("not historical execution evidence"), std::string::npos);
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
    trade_record a{};
    a.order_id = 1;
    a.side = order_side::buy;
    a.quantity = 1.0;
    a.fill_price = 10.0;
    a.intended_price = 10.0;
    a.timestamp = now;
    a.pnl = -50.0;
    a.symbol = "BTC";
    a.strategy_name = "sma";
    auto b = a;
    b.order_id = 2;
    b.side = order_side::sell;
    b.fill_price = 12.0;
    b.intended_price = 12.0;
    b.pnl = 30.0;
    auto c = b;
    c.order_id = 3;
    c.fill_price = 11.0;
    c.intended_price = 11.0;
    c.pnl = -10.0;
    r.trades = {a, b, c};

    std::string out = tt::render_worst_trades_section(r, {});
    auto p_minus50 = out.find("-50.00");
    auto p_minus10 = out.find("-10.00");
    ASSERT_NE(p_minus50, std::string::npos);
    ASSERT_NE(p_minus10, std::string::npos);
    EXPECT_LT(p_minus50, p_minus10);
}

TEST(ReportGenerator, InvalidTimeExposureIsVisiblyUnsupported)
{
    AnalyticsReport report;
    report.time_in_market_valid = false;
    report.time_in_market_reason = "non_monotonic_economic_time";

    const std::string out = tt::render_exposure_section(report, {});
    EXPECT_NE(out.find("unsupported"), std::string::npos);
    EXPECT_NE(out.find("non_monotonic_economic_time"), std::string::npos);
}

TEST(ReportGenerator, InvalidAnnualizationMakesCalmarVisiblyUnsupported)
{
    AnalyticsReport report;
    report.calmar_ratio = 0.0;
    report.calmar_ratio_valid = false;
    report.calmar_ratio_reason = "annualized_return_unavailable";

    const std::string out = tt::render_risk_section(report, {});
    EXPECT_NE(out.find("calmar ratio"), std::string::npos);
    EXPECT_NE(out.find("unsupported"), std::string::npos);
    EXPECT_NE(out.find("annualized_return_unavailable"), std::string::npos);
}

TEST(ReportGenerator, ProvisionalValuationAndInvalidBenchmarkAreVisible)
{
    AnalyticsReport report;
    report.valuation_complete = false;
    report.valuation_reason = "open_position_without_market_mark";
    report.benchmark_valid = false;
    report.benchmark_reason = "explicit_benchmark_required";

    const std::string out = tt::render_returns_section(report, {});
    EXPECT_NE(out.find("VALUATION"), std::string::npos);
    EXPECT_NE(out.find("open_position_without_market_mark"),
              std::string::npos);
    EXPECT_NE(out.find("buy & hold"), std::string::npos);
    EXPECT_NE(out.find("unsupported: explicit_benchmark_required"),
              std::string::npos);
}

TEST(ReportGenerator, AmbiguousPortfolioClockIsVisiblyUnsupported)
{
    AnalyticsReport report;
    report.portfolio_time_series_valid = false;
    report.portfolio_time_series_reason =
        "ambiguous_cross_symbol_arrival_without_watermark";

    const std::string out = tt::render_returns_section(report, {});
    EXPECT_NE(out.find("PORTFOLIO TIME SERIES"), std::string::npos);
    EXPECT_NE(out.find("ambiguous_cross_symbol_arrival_without_watermark"),
              std::string::npos);
}

TEST(ReportJsonContract, ExposesTimeMetricValidityAdditively)
{
    AnalyticsReport report;
    report.time_in_market_pct = 40.0;
    report.time_in_market_valid = true;
    report.time_in_market_reason = "computed_from_economic_time";
    report.duplicate_fill_replays_ignored = 2;
    report.conflicting_fill_replays_rejected = 3;
    report.missing_fill_identities_rejected = 4;
    report.invalid_fill_payloads_rejected = 5;
    report.unreconciled_funding_events_rejected = 6;
    report.duplicate_funding_replays_ignored = 7;
    report.conflicting_funding_replays_rejected = 8;
    report.late_fill_events_rejected = 9;
    report.late_funding_events_rejected = 10;
    report.late_market_events_rejected = 12;
    report.duplicate_market_marks_ignored = 13;
    report.conflicting_market_marks_rejected = 14;
    report.portfolio_time_series_valid = false;
    report.portfolio_time_series_reason =
        "ambiguous_cross_symbol_arrival_without_watermark";
    report.ambiguous_portfolio_mark_sequences_rejected = 11;
    trade_record fill{};
    fill.order_id = std::numeric_limits<std::uint64_t>::max();
    fill.side = order_side::buy;
    fill.quantity = 1.0;
    fill.fill_price = 100.0;
    fill.commission = -1.0;
    fill.commission_currency = "BNB";
    fill.intended_price = 100.0;
    fill.timestamp = std::chrono::system_clock::time_point{
        std::chrono::milliseconds{1234}};
    fill.symbol = "BTCUSDT";
    fill.strategy_name = "identity-test";
    fill.fill_id = 77;
    fill.venue_execution_id = "venue-exec-77";
    report.trades.push_back(fill);

    const std::string json = truetest::web::report_to_json(report);
    EXPECT_NE(json.find(
                  "\"order_id\":\"18446744073709551615\""),
              std::string::npos);
    EXPECT_NE(json.find("\"commission_currency\":\"BNB\""),
              std::string::npos);
    EXPECT_NE(json.find("\"schema_version\":1"), std::string::npos);
    EXPECT_NE(json.find("\"time_in_market_pct\":40"), std::string::npos);
    EXPECT_NE(json.find("\"time_in_market_valid\":true"),
              std::string::npos);
    EXPECT_NE(json.find(
                  "\"time_in_market_reason\":\"computed_from_economic_time\""),
              std::string::npos);
    EXPECT_NE(json.find("\"annualized_return_valid\":false"),
              std::string::npos);
    EXPECT_NE(json.find("\"sharpe_ratio_valid\":false"),
              std::string::npos);
    EXPECT_NE(json.find(
                  "\"sharpe_ratio_reason\":\"insufficient_return_observations\""),
              std::string::npos);
    EXPECT_NE(json.find("\"sortino_ratio_valid\":false"),
              std::string::npos);
    EXPECT_NE(json.find("\"calmar_ratio_valid\":false"),
              std::string::npos);
    EXPECT_NE(json.find("\"calmar_ratio_reason\":\"annualized_return_unavailable\""),
              std::string::npos);
    EXPECT_NE(json.find("\"benchmark_valid\":false"),
              std::string::npos);
    EXPECT_NE(json.find("\"benchmark_reason\":\"no_market_symbol\""),
              std::string::npos);
    EXPECT_NE(json.find("\"valuation_complete\":true"),
              std::string::npos);
    EXPECT_NE(json.find(
                  "\"valuation_reason\":\"all_open_positions_market_marked\""),
              std::string::npos);
    EXPECT_NE(json.find(
                  "\"annualized_return_basis\":\"causal_elapsed_time_365d\""),
              std::string::npos);
    EXPECT_NE(json.find("\"duplicate_fill_replays_ignored\":2"),
              std::string::npos);
    EXPECT_NE(json.find("\"conflicting_fill_replays_rejected\":3"),
              std::string::npos);
    EXPECT_NE(json.find("\"missing_fill_identities_rejected\":4"),
              std::string::npos);
    EXPECT_NE(json.find("\"invalid_fill_payloads_rejected\":5"),
              std::string::npos);
    EXPECT_NE(json.find("\"unreconciled_funding_events_rejected\":6"),
              std::string::npos);
    EXPECT_NE(json.find("\"duplicate_funding_replays_ignored\":7"),
              std::string::npos);
    EXPECT_NE(json.find("\"conflicting_funding_replays_rejected\":8"),
              std::string::npos);
    EXPECT_NE(json.find("\"late_fill_events_rejected\":9"),
              std::string::npos);
    EXPECT_NE(json.find("\"late_funding_events_rejected\":10"),
              std::string::npos);
    EXPECT_NE(json.find("\"late_market_events_rejected\":12"),
              std::string::npos);
    EXPECT_NE(json.find("\"duplicate_market_marks_ignored\":13"),
              std::string::npos);
    EXPECT_NE(json.find("\"conflicting_market_marks_rejected\":14"),
              std::string::npos);
    EXPECT_NE(json.find("\"portfolio_time_series_valid\":false"),
              std::string::npos);
    EXPECT_NE(json.find(
                  "\"portfolio_time_series_reason\":\"ambiguous_cross_symbol_arrival_without_watermark\""),
              std::string::npos);
    EXPECT_NE(json.find(
                  "\"ambiguous_portfolio_mark_sequences_rejected\":11"),
              std::string::npos);
    EXPECT_NE(json.find("\"closing_fill_legs\":0"), std::string::npos);
    EXPECT_NE(json.find("\"bankrupt\":false"), std::string::npos);
    EXPECT_NE(json.find("\"fee_model\":\"zero\""), std::string::npos);
    EXPECT_NE(json.find("\"execution_claim_scope\":\"not_synthetic_execution_claim\""),
              std::string::npos);
    EXPECT_NE(json.find("\"open_positions\":[]"), std::string::npos);
    EXPECT_NE(json.find("\"fill_id\":\"77\""), std::string::npos);
    EXPECT_NE(json.find("\"venue_execution_id\":\"venue-exec-77\""),
              std::string::npos);
}

TEST(ReportGenerator, RejectedEconomicEventsAreVisible)
{
    AnalyticsReport report;
    report.invalid_fill_payloads_rejected = 1;
    report.unreconciled_funding_events_rejected = 2;
    report.late_fill_events_rejected = 3;
    report.late_funding_events_rejected = 4;
    report.late_market_events_rejected = 5;
    report.duplicate_market_marks_ignored = 6;
    report.conflicting_market_marks_rejected = 7;

    const std::string out = tt::render_execution_section(report, {});
    EXPECT_NE(out.find("INVALID FILL PAYLOADS"), std::string::npos);
    EXPECT_NE(out.find("UNRECONCILED FUNDING EVENTS"), std::string::npos);
    EXPECT_NE(out.find("LATE FILL EVENTS"), std::string::npos);
    EXPECT_NE(out.find("LATE FUNDING EVENTS"), std::string::npos);
    EXPECT_NE(out.find("LATE MARKET EVENTS"), std::string::npos);
    EXPECT_NE(out.find("DUPLICATE MARKET MARKS"), std::string::npos);
    EXPECT_NE(out.find("CONFLICTING MARKET MARKS"), std::string::npos);
}
