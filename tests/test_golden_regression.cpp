// O2: Golden-file backtest regression tests.
// Runs a deterministic backtest against a checked-in fixture CSV and diffs
// the resulting metrics against a committed "golden" JSON file.
// The golden file (tests/golden/sma_basic_expected.json) was produced by
// running this same engine configuration once and is intentionally checked
// in. Any future code change that alters backtest outputs will fail this
// test, surfacing unintended behavior drift.
// Regenerating the golden file:
//   TRUETEST_REGENERATE_GOLDEN=1 ./build/truetest_tests \
//       --gtest_filter='GoldenRegression.*'
// After verifying the new numbers are intentional, commit the updated JSON.

#include <gtest/gtest.h>

#include "engine/engine.h"
#include "engine/engine_config.h"
#include "data/csv_data_source.h"
#include "data/data_handler.h"
#include "market_maker/market_maker.h"
#include "orderbook/orderbook.h"
#include "strategy/sma_strategy.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

namespace {

constexpr double kMetricTol = 1e-6;

struct silence_cout
{
    std::ostringstream sink;
    std::streambuf* orig;
    silence_cout() : sink(), orig(std::cout.rdbuf(sink.rdbuf())) {}
    ~silence_cout() { std::cout.rdbuf(orig); }
};

std::filesystem::path golden_dir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR).parent_path() / "golden";
}

// One deterministic backtest configuration. Keep in sync with
// tests/golden/sma_basic_config.json.
struct golden_run_result
{
    double final_equity;
    double cumulative_return;
    double max_drawdown;
    double sharpe_ratio;
    double sortino_ratio;
    double win_rate;
    double profit_factor;
    std::size_t total_trades;
    std::size_t total_orders;
    std::size_t total_fills;
    double buy_and_hold_return;
};

golden_run_result run_sma_basic()
{
    auto dh = std::make_shared<data_handler>();
    CsvDataSource src((golden_dir() / "sma_basic.csv").string());
    (void)src.load_data(dh);

    auto ob = std::make_shared<orderbook>();

    // Market maker: deterministic seed, 40 levels around $100
    MarketMaker mm(/*rng_seed=*/424242u + 1u);
    mm.add_orders(ob, 100.0, 40);

    auto strat = std::make_shared<sma_strategy>(/*period=*/5);

    engine_config cfg;
    cfg.initial_balance = 10000.0;
    cfg.seed            = 424242;
    cfg.threading       = thread_preset::inline_mode;
    cfg.disable_pinning = true;

    engine eng(dh, ob, strat, cfg);
    eng.run();

    const auto r = eng.get_analytics().generate_report();
    return {
        r.final_equity,
        r.cumulative_return,
        r.max_drawdown,
        r.sharpe_ratio,
        r.sortino_ratio,
        r.win_rate,
        r.profit_factor,
        r.total_trades,
        r.total_orders,
        r.total_fills,
        r.buy_and_hold_return,
    };
}

nlohmann::json to_json(const golden_run_result& r)
{
    return {
        {"final_equity",        r.final_equity},
        {"cumulative_return",   r.cumulative_return},
        {"max_drawdown",        r.max_drawdown},
        {"sharpe_ratio",        r.sharpe_ratio},
        {"sortino_ratio",       r.sortino_ratio},
        {"win_rate",            r.win_rate},
        {"profit_factor",       r.profit_factor},
        {"total_trades",        r.total_trades},
        {"total_orders",        r.total_orders},
        {"total_fills",         r.total_fills},
        {"buy_and_hold_return", r.buy_and_hold_return},
    };
}

bool regenerate_mode()
{
    const char* env = std::getenv("TRUETEST_REGENERATE_GOLDEN");
    return env != nullptr && env[0] != '\0' && env[0] != '0';
}

}


TEST(GoldenRegression, SmaBasic)
{
    silence_cout quiet;

    const auto golden_path = golden_dir() / "sma_basic_expected.json";

    // Two consecutive runs should be identical - a prerequisite for any
    // meaningful golden comparison.
    auto first  = run_sma_basic();
    auto second = run_sma_basic();
    EXPECT_DOUBLE_EQ(first.final_equity, second.final_equity);
    EXPECT_DOUBLE_EQ(first.cumulative_return, second.cumulative_return);
    EXPECT_EQ       (first.total_fills, second.total_fills);

    if (regenerate_mode())
    {
        std::ofstream f(golden_path);
        ASSERT_TRUE(f.is_open()) << "Cannot write " << golden_path;
        f << to_json(first).dump(2) << "\n";
        std::cerr << "[golden] regenerated " << golden_path << "\n";
        return;  // treat regeneration as success so CI doesn't report skip
    }

    ASSERT_TRUE(std::filesystem::exists(golden_path))
        << "Golden file missing: " << golden_path
        << "\nRun with TRUETEST_REGENERATE_GOLDEN=1 to create it.";

    nlohmann::json expected;
    {
        std::ifstream f(golden_path);
        ASSERT_TRUE(f.is_open());
        f >> expected;
    }

    EXPECT_NEAR(first.final_equity,       expected["final_equity"].get<double>(),       kMetricTol);
    EXPECT_NEAR(first.cumulative_return,  expected["cumulative_return"].get<double>(),  kMetricTol);
    EXPECT_NEAR(first.max_drawdown,       expected["max_drawdown"].get<double>(),       kMetricTol);
    EXPECT_NEAR(first.sharpe_ratio,       expected["sharpe_ratio"].get<double>(),       kMetricTol);
    EXPECT_NEAR(first.sortino_ratio,      expected["sortino_ratio"].get<double>(),      kMetricTol);
    EXPECT_NEAR(first.win_rate,           expected["win_rate"].get<double>(),           kMetricTol);
    EXPECT_NEAR(first.profit_factor,      expected["profit_factor"].get<double>(),      kMetricTol);
    EXPECT_NEAR(first.buy_and_hold_return, expected["buy_and_hold_return"].get<double>(), kMetricTol);
    EXPECT_EQ  (first.total_trades,       expected["total_trades"].get<std::size_t>());
    EXPECT_EQ  (first.total_orders,       expected["total_orders"].get<std::size_t>());
    EXPECT_EQ  (first.total_fills,        expected["total_fills"].get<std::size_t>());
}
