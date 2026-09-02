// Plan 07 metamorphic backtest-validity contracts.  These tests deliberately
// exercise only the deterministic backtest path; campaign-level byte identity
// of JSON and binary event ledgers is enforced by sensitivity_oos_contract.

#include <gtest/gtest.h>

#include "analytics/analytics.h"
#include "data/market_series.h"
#include "engine/engine.h"
#include "engine/engine_config.h"
#include "market_maker/market_maker.h"
#include "orderbook/orderbook.h"
#include "providers/local/csv_parser.h"
#include "strategy/sma/sma_strategy.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct silence_output
{
    std::ostringstream cout_sink;
    std::ostringstream cerr_sink;
    std::streambuf* cout_original = std::cout.rdbuf(cout_sink.rdbuf());
    std::streambuf* cerr_original = std::cerr.rdbuf(cerr_sink.rdbuf());
    ~silence_output()
    {
        std::cout.rdbuf(cout_original);
        std::cerr.rdbuf(cerr_original);
    }
};

struct parsed_fixture
{
    std::vector<bar_record> rows;
    std::vector<tt::data_provenance::rejected_row> rejected;
};

std::filesystem::path golden_csv()
{
    return std::filesystem::path(TEST_FIXTURES_DIR).parent_path() / "golden" / "sma_basic.csv";
}

std::string whitespace_only_format(std::string_view line)
{
    std::string out{" \t"};
    for (const char c : line) {
        if (c == ',')
            out += " , ";
        else
            out.push_back(c);
    }
    out += " \r";
    return out;
}

parsed_fixture parse_golden(bool whitespace_format, bool insert_repeated_header)
{
    std::ifstream file(golden_csv());
    if (!file.is_open()) throw std::runtime_error("cannot open SMA metamorphic fixture");

    std::string header;
    std::getline(file, header);
    CsvBarParser parser;
    if (!parser.parse_header(whitespace_format ? whitespace_only_format(header) : header))
        throw std::runtime_error("cannot parse SMA metamorphic header");

    parsed_fixture result;
    std::string line;
    std::size_t source_row = 0;
    while (std::getline(file, line)) {
        if (insert_repeated_header && source_row == 10) {
            EXPECT_FALSE(
                parser.parse_record(whitespace_format ? whitespace_only_format(header) : header));
        }

        const auto parsed =
            parser.parse_record(whitespace_format ? whitespace_only_format(line) : line);
        if (!parsed) throw std::runtime_error("fixture row rejected unexpectedly");
        result.rows.push_back(*parsed);
        ++source_row;
    }
    result.rejected.assign(parser.rejections().begin(), parser.rejections().end());
    return result;
}

AnalyticsReport run_sma(const std::vector<bar_record>& rows, double price_scale,
                        std::size_t periods_per_year = 252)
{
    auto data = std::make_shared<data_handler>();
    for (const auto& row : rows) {
        if (!data->load_into_queue(row.date, row.symbol, row.open * price_scale,
                                   row.high * price_scale, row.low * price_scale,
                                   row.close * price_scale, row.volume, row.quantity_scale)) {
            throw std::runtime_error("metamorphic fixture failed domain validation");
        }
    }

    auto book = std::make_shared<orderbook>();
    MarketMaker maker(/*rng_seed=*/424243u);
    maker.add_orders(book, 100.0 * price_scale, 40);

    auto strategy = std::make_shared<sma_strategy>(/*period=*/5);
    engine_config config;
    config.initial_balance = 10000.0 * price_scale;
    config.seed = 424242;
    config.threading = thread_preset::inline_mode;
    config.disable_pinning = true;
    config.periods_per_year = periods_per_year;
    config.exit_defaults.mode = truetest::exits::exit_policy_mode::strategy_only;

    engine eng(data, book, strategy, config);
    eng.run();
    return eng.get_analytics().generate_report();
}

void expect_equivalent_trade_ledger(const AnalyticsReport& lhs, const AnalyticsReport& rhs)
{
    EXPECT_EQ(lhs.total_orders, rhs.total_orders);
    EXPECT_EQ(lhs.total_fills, rhs.total_fills);
    ASSERT_EQ(lhs.trades.size(), rhs.trades.size());
    for (std::size_t i = 0; i < lhs.trades.size(); ++i) {
        const auto& a = lhs.trades[i];
        const auto& b = rhs.trades[i];
        // IDs are process-global allocator identities.  The script-level
        // contract checks byte identity in fresh processes; this in-process
        // metamorphic test compares the economic sequence they identify.
        EXPECT_EQ(a.side, b.side);
        EXPECT_EQ(a.timestamp, b.timestamp);
        EXPECT_EQ(a.reference_timestamp, b.reference_timestamp);
        EXPECT_DOUBLE_EQ(a.quantity, b.quantity);
        EXPECT_DOUBLE_EQ(a.fill_price, b.fill_price);
        EXPECT_DOUBLE_EQ(a.intended_price, b.intended_price);
        EXPECT_DOUBLE_EQ(a.reference_price, b.reference_price);
        EXPECT_DOUBLE_EQ(a.commission, b.commission);
        EXPECT_DOUBLE_EQ(a.pnl, b.pnl);
        EXPECT_EQ(a.symbol, b.symbol);
        EXPECT_EQ(a.strategy_name, b.strategy_name);
    }
}

void expect_equivalent_trade_artifacts(const AnalyticsReport& lhs, const AnalyticsReport& rhs)
{
    expect_equivalent_trade_ledger(lhs, rhs);
    EXPECT_DOUBLE_EQ(lhs.final_equity, rhs.final_equity);
    EXPECT_DOUBLE_EQ(lhs.realized_pnl, rhs.realized_pnl);
    EXPECT_DOUBLE_EQ(lhs.unrealized_pnl, rhs.unrealized_pnl);
    EXPECT_DOUBLE_EQ(lhs.cumulative_return, rhs.cumulative_return);
    EXPECT_DOUBLE_EQ(lhs.max_drawdown, rhs.max_drawdown);
    EXPECT_DOUBLE_EQ(lhs.sharpe_ratio, rhs.sharpe_ratio);
}

std::vector<std::optional<order_side>> sma_signal_sides(const std::vector<double>& closes)
{
    sma_strategy strategy(/*period=*/3);
    std::vector<std::optional<order_side>> result;
    result.reserve(closes.size());
    for (std::size_t i = 0; i < closes.size(); ++i) {
        const auto timestamp = std::chrono::system_clock::time_point{
            std::chrono::milliseconds{static_cast<std::int64_t>(i)}};
        const auto signal = strategy.on_market(market_event(timestamp, "METAMORPHIC", closes[i],
                                                            closes[i], closes[i], closes[i], 100));
        result.push_back(signal ? std::optional<order_side>{signal->get_side()} : std::nullopt);
    }
    return result;
}

}  // namespace

TEST(SensitivityMetamorphic, SameSeedAndInputProduceIdenticalTradeArtifacts)
{
    silence_output quiet;
    const auto rows = parse_golden(/*whitespace_format=*/false,
                                   /*insert_repeated_header=*/false)
                          .rows;
    const auto first = run_sma(rows, /*price_scale=*/1.0);
    const auto second = run_sma(rows, /*price_scale=*/1.0);

    ASSERT_FALSE(first.trades.empty());
    expect_equivalent_trade_artifacts(first, second);
}

TEST(SensitivityMetamorphic, PeriodsPerYearChangesOnlyAnnualizedAnalytics)
{
    silence_output quiet;
    const auto rows = parse_golden(/*whitespace_format=*/false,
                                   /*insert_repeated_header=*/false)
                          .rows;
    const auto crypto_minutes = run_sma(rows, /*price_scale=*/1.0, 525600);
    const auto daily = run_sma(rows, /*price_scale=*/1.0, 252);

    ASSERT_FALSE(crypto_minutes.trades.empty());
    expect_equivalent_trade_ledger(crypto_minutes, daily);
    EXPECT_DOUBLE_EQ(crypto_minutes.final_equity, daily.final_equity);
    EXPECT_DOUBLE_EQ(crypto_minutes.realized_pnl, daily.realized_pnl);
    EXPECT_DOUBLE_EQ(crypto_minutes.unrealized_pnl, daily.unrealized_pnl);
    EXPECT_DOUBLE_EQ(crypto_minutes.cumulative_return, daily.cumulative_return);
    EXPECT_DOUBLE_EQ(crypto_minutes.max_drawdown, daily.max_drawdown);
    EXPECT_NE(crypto_minutes.annualized_return, daily.annualized_return);
    EXPECT_NE(crypto_minutes.sharpe_ratio, daily.sharpe_ratio);
    EXPECT_NE(crypto_minutes.sortino_ratio, daily.sortino_ratio);
    EXPECT_NE(crypto_minutes.calmar_ratio, daily.calmar_ratio);
}

TEST(SensitivityMetamorphic, PriceAndBalanceScalePreserveTradeSequenceAndRelativeMetrics)
{
    silence_output quiet;
    const auto rows = parse_golden(/*whitespace_format=*/false,
                                   /*insert_repeated_header=*/false)
                          .rows;
    const auto base = run_sma(rows, /*price_scale=*/1.0);
    const auto doubled = run_sma(rows, /*price_scale=*/2.0);

    ASSERT_FALSE(base.trades.empty());
    ASSERT_EQ(base.trades.size(), doubled.trades.size());
    EXPECT_EQ(base.total_orders, doubled.total_orders);
    EXPECT_EQ(base.total_fills, doubled.total_fills);
    // Resting-book price quantization and adaptive risk sizing make exact
    // floating-point doubling too strict.  The bounds are far below one bp
    // of account equity and pin the intended scale relation without hiding a
    // meaningful economic drift.
    EXPECT_NEAR(doubled.final_equity, base.final_equity * 2.0, 0.05);
    EXPECT_NEAR(doubled.realized_pnl, base.realized_pnl * 2.0, 0.05);
    EXPECT_NEAR(doubled.unrealized_pnl, base.unrealized_pnl * 2.0, 0.05);
    EXPECT_NEAR(doubled.cumulative_return, base.cumulative_return, 2e-6);
    EXPECT_NEAR(doubled.max_drawdown, base.max_drawdown, 2e-4);
    EXPECT_NEAR(doubled.sharpe_ratio, base.sharpe_ratio, 3e-4);

    for (std::size_t i = 0; i < base.trades.size(); ++i) {
        const auto& a = base.trades[i];
        const auto& b = doubled.trades[i];
        EXPECT_EQ(a.side, b.side) << "trade index " << i;
        EXPECT_EQ(a.timestamp, b.timestamp) << "trade index " << i;
        EXPECT_NEAR(a.quantity, b.quantity, 2e-4) << "trade index " << i;
        EXPECT_NEAR(b.fill_price, a.fill_price * 2.0, 3e-4) << "trade index " << i;
        EXPECT_NEAR(b.intended_price, a.intended_price * 2.0, 3e-4) << "trade index " << i;
        EXPECT_NEAR(b.pnl, a.pnl * 2.0, 0.05) << "trade index " << i;
    }
}

TEST(SensitivityMetamorphic, CsvFormattingAndRepeatedHeaderLeaveLaterArtifactsStable)
{
    silence_output quiet;
    const auto plain = parse_golden(/*whitespace_format=*/false,
                                    /*insert_repeated_header=*/false);
    const auto formatted = parse_golden(/*whitespace_format=*/true,
                                        /*insert_repeated_header=*/false);
    const auto repeated_header = parse_golden(/*whitespace_format=*/false,
                                              /*insert_repeated_header=*/true);

    ASSERT_EQ(plain.rows.size(), formatted.rows.size());
    ASSERT_EQ(plain.rows.size(), repeated_header.rows.size());
    ASSERT_EQ(repeated_header.rejected.size(), 1u);
    EXPECT_EQ(repeated_header.rejected.front().reason,
              tt::data_provenance::rejection_reason::repeated_header);
    for (std::size_t i = 0; i < plain.rows.size(); ++i) {
        ASSERT_TRUE(plain.rows[i].source.has_value());
        ASSERT_TRUE(formatted.rows[i].source.has_value());
        ASSERT_TRUE(repeated_header.rows[i].source.has_value());
        EXPECT_EQ(plain.rows[i].source->accepted_index, i);
        EXPECT_EQ(formatted.rows[i].source->accepted_index, i);
        EXPECT_EQ(repeated_header.rows[i].source->accepted_index, i);
        EXPECT_DOUBLE_EQ(plain.rows[i].close, formatted.rows[i].close);
        EXPECT_DOUBLE_EQ(plain.rows[i].close, repeated_header.rows[i].close);
    }

    const auto plain_report = run_sma(plain.rows, 1.0);
    const auto formatted_report = run_sma(formatted.rows, 1.0);
    const auto repeated_header_report = run_sma(repeated_header.rows, 1.0);
    expect_equivalent_trade_artifacts(plain_report, formatted_report);
    expect_equivalent_trade_artifacts(plain_report, repeated_header_report);
}

TEST(SensitivityMetamorphic, OneBarFeatureShiftHasNoPriorSignalAndAgesOutOfSmaWindow)
{
    const std::vector<double> baseline{100.0, 100.0, 100.0, 100.0, 100.0, 100.0, 100.0};
    const std::vector<double> shifted{100.0, 100.0, 100.0, 101.0, 100.0, 100.0, 100.0};
    const auto baseline_signals = sma_signal_sides(baseline);
    const auto shifted_signals = sma_signal_sides(shifted);

    ASSERT_EQ(baseline_signals.size(), shifted_signals.size());
    for (std::size_t i = 0; i < 3; ++i)
        EXPECT_EQ(shifted_signals[i], baseline_signals[i]) << "pre-feature bar " << i;

    ASSERT_TRUE(shifted_signals[3].has_value());
    EXPECT_EQ(*shifted_signals[3], order_side::buy)
        << "an upward feature shift may create an upward SMA crossing only at the feature bar";
    ASSERT_TRUE(shifted_signals[4].has_value());
    EXPECT_EQ(*shifted_signals[4], order_side::sell)
        << "the reversal is the bounded causal consequence of the shifted SMA window";
    for (std::size_t i = 5; i < shifted_signals.size(); ++i)
        EXPECT_EQ(shifted_signals[i], baseline_signals[i]) << "post-window bar " << i;
}
