#include <gtest/gtest.h>

#include "api/truetest_api.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <limits>
#include <string>
#include <thread>

namespace {

std::string fixture_path(const char* name)
{
    return (std::filesystem::path{TEST_FIXTURES_DIR} / name).string();
}

class api_handle
{
public:
    explicit api_handle(const nlohmann::json& config)
        : value_(tt_create_engine(config.dump().c_str()))
    {}

    ~api_handle() { tt_destroy(value_); }
    api_handle(const api_handle&) = delete;
    api_handle& operator=(const api_handle&) = delete;

    tt_engine_handle get() const noexcept { return value_; }

private:
    tt_engine_handle value_ = nullptr;
};

}  // namespace

TEST(TrueTestApiContract, InvalidExpectedSymbolIsRejectedAtCreate)
{
    nlohmann::json config = {
        {"data_path", fixture_path("binance_kline_sample.csv")},
        {"symbol", "BTC USDT"},
    };

    api_handle handle(config);

    EXPECT_EQ(handle.get(), nullptr);
    EXPECT_NE(std::string{tt_last_error()}.find("symbol"), std::string::npos);
}

TEST(TrueTestApiContract, StrictSchemaRejectsUnknownWrongTypedAndOutOfRangeFields)
{
    const auto path = fixture_path("binance_kline_sample.csv");
    const std::array<nlohmann::json, 17> invalid_configs = {
        nlohmann::json::array({path}),
        nlohmann::json{{"data_path", path}, {"sybmol", "BTCUSDT"}},
        nlohmann::json{{"data_path", path}, {"strategy", 7}},
        nlohmann::json{{"data_path", path}, {"params", 7}},
        nlohmann::json{{"data_path", path}, {"params", {{"length", "7"}}}},
        nlohmann::json{{"data_path", ""}},
        nlohmann::json{{"data_path", path}, {"initial_balance", 0.0}},
        nlohmann::json{{"data_path", path}, {"seed", -1}},
        nlohmann::json{{"data_path", path}, {"rolling_window", 0}},
        nlohmann::json{{"data_path", path}, {"periods_per_year", 0}},
        nlohmann::json{{"data_path", path}, {"market_aggression", 1.0}},
        nlohmann::json{{"data_path", path}, {"market_aggression", 2.0}},
        nlohmann::json{{"data_path", path}, {"qty_scale", 100'000'000.0}},
        nlohmann::json{{"data_path", path}, {"spread_step_factor", 0.0001}},
        nlohmann::json{{"data_path", path}, {"risk_free_rate", "0.01"}},
        nlohmann::json{{"data_path", path}, {"seed", 7}, {"fill_rng_seed", 9}},
        nlohmann::json{{"data_path", path},
                       {"fill_rng_seed",
                        static_cast<std::uint64_t>(std::numeric_limits<unsigned>::max()) + 1u}},
    };

    for (std::size_t i = 0; i < invalid_configs.size(); ++i) {
        SCOPED_TRACE(i);
        api_handle handle(invalid_configs[i]);
        EXPECT_EQ(handle.get(), nullptr);
        EXPECT_FALSE(std::string{tt_last_error()}.empty());
    }

    nlohmann::json max_seed = {
        {"data_path", path},
        {"seed", std::numeric_limits<std::uint64_t>::max()},
    };
    api_handle max_seed_handle(max_seed);
    EXPECT_NE(max_seed_handle.get(), nullptr) << tt_last_error();
}

TEST(TrueTestApiContract, MissingDeterministicMasterSeedIsRejectedAtCreate)
{
    api_handle handle(nlohmann::json{
        {"data_path", fixture_path("binance_kline_sample.csv")}});
    EXPECT_EQ(handle.get(), nullptr);
    EXPECT_NE(std::string{tt_last_error()}.find("explicit deterministic seed"),
              std::string::npos);
}

TEST(TrueTestApiContract, StrictSchemaRejectsDuplicateKeysAtEveryObjectDepth)
{
    const auto quoted_path = nlohmann::json(fixture_path("binance_kline_sample.csv")).dump();
    const std::array<std::string, 3> duplicate_configs = {
        "{\"data_path\":" + quoted_path + ",\"data_path\":" + quoted_path + "}",
        "{\"data_path\":" + quoted_path + ",\"params\":{\"length\":7,\"length\":8}}",
        "{\"data_path\":" + quoted_path + ",\"params\":{\"\":1,\"\":2}}",
    };

    for (const auto& config : duplicate_configs) {
        tt_engine_handle handle = tt_create_engine(config.c_str());
        EXPECT_EQ(handle, nullptr);
        EXPECT_NE(std::string{tt_last_error()}.find("duplicate"), std::string::npos);
        tt_destroy(handle);
    }
}

TEST(TrueTestApiContract, UnboundDatasetFailureIsIdempotentAndKeepsResultsClosed)
{
    nlohmann::json config = {
        {"data_path", fixture_path("binance_kline_sample.csv")},
        {"seed", 0},
    };
    api_handle handle(config);
    ASSERT_NE(handle.get(), nullptr) << tt_last_error();

    EXPECT_EQ(tt_run(handle.get()), 3);
    const std::string first_error = tt_last_error();
    EXPECT_NE(first_error.find("instrument symbol"), std::string::npos);
    EXPECT_EQ(tt_run(handle.get()), 3);
    EXPECT_EQ(tt_last_error(), first_error);
    EXPECT_EQ(tt_get_results(handle.get()), nullptr);
}

TEST(TrueTestApiContract, BoundDatasetSuccessfulRunIsIdempotent)
{
    nlohmann::json config = {
        {"data_path", fixture_path("binance_kline_sample.csv")},
        {"strategy", "sma"},
        {"symbol", "BTCUSDT"},
        {"seed", 424242},
    };
    api_handle handle(config);
    ASSERT_NE(handle.get(), nullptr) << tt_last_error();

    EXPECT_EQ(tt_run(handle.get()), 0) << tt_last_error();
    EXPECT_EQ(tt_run(handle.get()), 0) << tt_last_error();
    std::unique_ptr<const char, decltype(&tt_free_string)> report{tt_get_results(handle.get()),
                                                                  &tt_free_string};
    ASSERT_TRUE(report) << tt_last_error();
    const auto parsed = nlohmann::json::parse(report.get());
    EXPECT_TRUE(parsed.is_object());
    ASSERT_TRUE(parsed.contains("schema_version"));
    EXPECT_EQ(parsed["schema_version"], 1);
    EXPECT_EQ(parsed.value("trade_rows_kind", ""), "physical_fill_legs");
    EXPECT_EQ(parsed.value("trade_returns_kind", ""), "closing_fill_legs");
    ASSERT_TRUE(parsed.contains("trades"));
    EXPECT_TRUE(parsed["trades"].is_array());
    ASSERT_TRUE(parsed.contains("trade_returns"));
    EXPECT_TRUE(parsed["trade_returns"].is_array());
    ASSERT_TRUE(parsed.contains("winning_trades"));
    EXPECT_TRUE(parsed["winning_trades"].is_number_unsigned());
    for (const char* field : {"gross_realized_pnl", "realized_pnl",
                              "funding_pnl", "unrealized_pnl",
                              "total_commission", "reconciliation_residual"}) {
        EXPECT_TRUE(parsed.contains(field)) << field;
        EXPECT_TRUE(parsed[field].is_number()) << field;
    }
    EXPECT_TRUE(parsed.contains("accounting_reconciled"));
    EXPECT_TRUE(parsed["accounting_reconciled"].is_boolean());
    EXPECT_TRUE(parsed.contains("accounting_reconciliation_reason"));
    EXPECT_TRUE(parsed["accounting_reconciliation_reason"].is_string());
    for (const char* field : {"avg_holding_period_ms", "avg_slippage_signed",
                              "avg_adverse_slippage", "avg_favorable_slippage",
                              "adverse_slippage_count", "favorable_slippage_count",
                              "avg_tick_to_trade_ns", "min_tick_to_trade_ns",
                              "max_tick_to_trade_ns", "tick_to_trade_samples"}) {
        EXPECT_TRUE(parsed.contains(field)) << field;
    }
    for (const char* breakdown : {"per_symbol", "per_strategy"}) {
        ASSERT_TRUE(parsed.contains(breakdown));
        ASSERT_TRUE(parsed[breakdown].is_object());
        for (const auto& [_, value] : parsed[breakdown].items())
        {
            EXPECT_TRUE(value.contains("win_count"));
            EXPECT_TRUE(value.contains("total_win"));
            EXPECT_TRUE(value.contains("total_loss"));
            EXPECT_TRUE(value.contains("profit_factor_valid"));
            EXPECT_TRUE(value.contains("profit_factor_unbounded"));
            EXPECT_TRUE(value.contains("profit_factor_reason"));
        }
    }
    EXPECT_TRUE(parsed.contains("total_win"));
    EXPECT_TRUE(parsed.contains("total_loss"));
    EXPECT_TRUE(parsed.contains("profit_factor_valid"));
    EXPECT_TRUE(parsed.contains("profit_factor_unbounded"));
    EXPECT_TRUE(parsed.contains("profit_factor_reason"));
    EXPECT_TRUE(parsed.contains("time_in_market_valid"));
    EXPECT_TRUE(parsed["time_in_market_valid"].is_boolean());
    EXPECT_TRUE(parsed.contains("time_in_market_reason"));
    EXPECT_TRUE(parsed["time_in_market_reason"].is_string());
    EXPECT_TRUE(parsed.contains("annualized_return"));
    EXPECT_TRUE(parsed["annualized_return"].is_number());
    EXPECT_TRUE(parsed.contains("annualized_return_valid"));
    EXPECT_TRUE(parsed["annualized_return_valid"].is_boolean());
    EXPECT_TRUE(parsed.contains("sharpe_ratio_valid"));
    EXPECT_TRUE(parsed["sharpe_ratio_valid"].is_boolean());
    EXPECT_TRUE(parsed.contains("sharpe_ratio_reason"));
    EXPECT_TRUE(parsed["sharpe_ratio_reason"].is_string());
    EXPECT_TRUE(parsed.contains("sortino_ratio_valid"));
    EXPECT_TRUE(parsed["sortino_ratio_valid"].is_boolean());
    EXPECT_TRUE(parsed.contains("sortino_ratio_reason"));
    EXPECT_TRUE(parsed["sortino_ratio_reason"].is_string());
    EXPECT_TRUE(parsed.contains("calmar_ratio_valid"));
    EXPECT_TRUE(parsed["calmar_ratio_valid"].is_boolean());
    EXPECT_TRUE(parsed.contains("calmar_ratio_reason"));
    EXPECT_TRUE(parsed["calmar_ratio_reason"].is_string());
    EXPECT_TRUE(parsed.contains("benchmark_valid"));
    EXPECT_TRUE(parsed["benchmark_valid"].is_boolean());
    EXPECT_TRUE(parsed.contains("benchmark_reason"));
    EXPECT_TRUE(parsed["benchmark_reason"].is_string());
    EXPECT_TRUE(parsed.contains("benchmark_symbol"));
    EXPECT_TRUE(parsed["benchmark_symbol"].is_string());
    EXPECT_TRUE(parsed.contains("benchmark_equity_curve"));
    ASSERT_TRUE(parsed.contains("benchmark_equity_curve_sample_stride"));
    EXPECT_TRUE(parsed["benchmark_equity_curve_sample_stride"].is_number_unsigned());
    EXPECT_GE(parsed["benchmark_equity_curve_sample_stride"].get<std::size_t>(), 1u);
    ASSERT_TRUE(parsed.contains("benchmark_curve_observation_basis"));
    EXPECT_EQ(parsed["benchmark_curve_observation_basis"],
              "selected_symbol_market_marks");
    ASSERT_TRUE(parsed["benchmark_equity_curve"].is_array());
    ASSERT_FALSE(parsed["benchmark_equity_curve"].empty());
    for (const auto& point : parsed["benchmark_equity_curve"])
    {
        ASSERT_TRUE(point.is_array());
        ASSERT_EQ(point.size(), 2u);
        EXPECT_TRUE(point[0].is_number_integer());
        EXPECT_TRUE(point[1].is_number());
        if (point[1].is_number())
            EXPECT_TRUE(std::isfinite(point[1].get<double>()));
    }
    const auto& benchmark_curve = parsed["benchmark_equity_curve"];
    EXPECT_EQ(benchmark_curve.front()[0].get<std::int64_t>(),
              1'577'836'859'999LL);
    EXPECT_DOUBLE_EQ(benchmark_curve.front()[1].get<double>(), 100'000.0);
    for (std::size_t i = 1; i < benchmark_curve.size(); ++i)
        EXPECT_LT(benchmark_curve[i - 1][0].get<std::int64_t>(),
                  benchmark_curve[i][0].get<std::int64_t>());
    EXPECT_TRUE(parsed.contains("valuation_complete"));
    EXPECT_TRUE(parsed["valuation_complete"].is_boolean());
    EXPECT_TRUE(parsed.contains("valuation_reason"));
    EXPECT_TRUE(parsed["valuation_reason"].is_string());
    EXPECT_TRUE(parsed.contains("portfolio_time_series_valid"));
    EXPECT_TRUE(parsed["portfolio_time_series_valid"].is_boolean());
    EXPECT_TRUE(parsed.contains("portfolio_time_series_reason"));
    EXPECT_TRUE(parsed["portfolio_time_series_reason"].is_string());
    EXPECT_TRUE(parsed.contains(
        "ambiguous_portfolio_mark_sequences_rejected"));
    EXPECT_TRUE(parsed[
        "ambiguous_portfolio_mark_sequences_rejected"].is_number_unsigned());
    EXPECT_TRUE(parsed.contains("duplicate_fill_replays_ignored"));
    EXPECT_TRUE(parsed["duplicate_fill_replays_ignored"].is_number_unsigned());
    EXPECT_TRUE(parsed.contains("conflicting_fill_replays_rejected"));
    EXPECT_TRUE(parsed["conflicting_fill_replays_rejected"].is_number_unsigned());
    EXPECT_TRUE(parsed.contains("missing_fill_identities_rejected"));
    EXPECT_TRUE(parsed["missing_fill_identities_rejected"].is_number_unsigned());
    EXPECT_TRUE(parsed.contains("invalid_fill_payloads_rejected"));
    EXPECT_TRUE(parsed["invalid_fill_payloads_rejected"].is_number_unsigned());
    EXPECT_TRUE(parsed.contains("unreconciled_funding_events_rejected"));
    EXPECT_TRUE(parsed["unreconciled_funding_events_rejected"].is_number_unsigned());
    EXPECT_TRUE(parsed.contains("duplicate_funding_replays_ignored"));
    EXPECT_TRUE(parsed["duplicate_funding_replays_ignored"].is_number_unsigned());
    EXPECT_TRUE(parsed.contains("conflicting_funding_replays_rejected"));
    EXPECT_TRUE(parsed["conflicting_funding_replays_rejected"].is_number_unsigned());
    EXPECT_TRUE(parsed.contains("late_fill_events_rejected"));
    EXPECT_TRUE(parsed["late_fill_events_rejected"].is_number_unsigned());
    EXPECT_TRUE(parsed.contains("late_funding_events_rejected"));
    EXPECT_TRUE(parsed["late_funding_events_rejected"].is_number_unsigned());
    EXPECT_TRUE(parsed.contains("late_market_events_rejected"));
    EXPECT_TRUE(parsed["late_market_events_rejected"].is_number_unsigned());
    EXPECT_TRUE(parsed.contains("duplicate_market_marks_ignored"));
    EXPECT_TRUE(parsed["duplicate_market_marks_ignored"].is_number_unsigned());
    EXPECT_TRUE(parsed.contains("conflicting_market_marks_rejected"));
    EXPECT_TRUE(parsed["conflicting_market_marks_rejected"].is_number_unsigned());
    EXPECT_TRUE(parsed.contains("annualized_return_reason"));
    EXPECT_TRUE(parsed["annualized_return_reason"].is_string());
    EXPECT_EQ(parsed.value("annualized_return_basis", ""),
              "causal_elapsed_time_365d");
}

TEST(TrueTestApiContract, ConcurrentCallsShareTheFirstCompletedResult)
{
    nlohmann::json config = {
        {"data_path", fixture_path("binance_kline_sample.csv")},
        {"strategy", "sma"},
        {"symbol", "BTCUSDT"},
        {"seed", 424242},
    };
    api_handle handle(config);
    ASSERT_NE(handle.get(), nullptr) << tt_last_error();

    std::array<int, 8> results{};
    std::array<std::thread, 8> threads;
    for (std::size_t i = 0; i < threads.size(); ++i)
        threads[i] = std::thread([&, i] { results[i] = tt_run(handle.get()); });
    for (auto& thread : threads)
        thread.join();

    for (const int result : results)
        EXPECT_EQ(result, 0);
}
