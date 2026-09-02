#include <gtest/gtest.h>
#include "engine/engine.h"
#include "engine/engine_config.h"
#include "data/data_handler.h"
#include "orderbook/orderbook.h"
#include "market_maker/market_maker.h"
#include <sstream>

// Silence stdout during engine runs.
// Anonymous namespace avoids ODR clashes with the same helper in other TUs.
namespace {
struct SilenceCout {
    std::ostringstream sink;
    std::streambuf* orig;
    SilenceCout() : sink(), orig(std::cout.rdbuf(sink.rdbuf())) {}
    ~SilenceCout() { std::cout.rdbuf(orig); }
};
}

// Strategy: buys on bar 3, sells on bar 6 (deterministic)
class CorrectnessStrategy : public IStrategy
{
    bool position_open_ = false;
    int call_count_ = 0;
public:
    int get_call_count() const { return call_count_; }

    std::optional<order_event> on_market(const market_event& mkt) override
    {
        call_count_++;
        if (call_count_ == 3 && !position_open_)
        {
            return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                               order_type::market, order_side::buy, 10, mkt.get_close());
        }
        if (call_count_ == 6 && position_open_)
        {
            return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                               order_type::market, order_side::sell, 10, mkt.get_close());
        }
        return std::nullopt;
    }
    void set_position_open(const std::string&, bool open) override { position_open_ = open; }
};

static std::shared_ptr<data_handler> make_test_data(int n)
{
    auto dh = std::make_shared<data_handler>();
    for (int i = 0; i < n; ++i)
        dh->load_into_queue("2024-01-01", "TEST",
                            100.0 + i, 105.0 + i, 95.0 + i, 102.0 + i, 1000);
    return dh;
}

struct PresetResult
{
    std::size_t trades;
    int strategy_calls;
};

static PresetResult run_preset(thread_preset preset, int bars = 100)
{
    SilenceCout quiet;
    auto dh = make_test_data(bars);
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<CorrectnessStrategy>();

    MarketMaker mm(42);
    mm.add_orders(ob, 100.0, 10);

    engine_config cfg;
    cfg.threading = preset;
    cfg.seed = 42;
    cfg.disable_pinning = true;

    engine eng(dh, ob, strat, std::move(cfg));
    eng.run();

    PresetResult r;
    r.strategy_calls = strat->get_call_count();
    r.trades = bars;  // all bars processed
    return r;
}

TEST(ThreadingCorrectness, AllPresetsProduceSameStrategyCallCount)
{
    const int bars = 100;
    auto inline_result = run_preset(thread_preset::inline_mode, bars);
    auto logging_result = run_preset(thread_preset::logging_only, bars);
    auto light_result = run_preset(thread_preset::light, bars);
    auto standard_result = run_preset(thread_preset::standard, bars);
    auto full_result = run_preset(thread_preset::full, bars);
    auto extended_result = run_preset(thread_preset::extended, bars);

    // All presets should process the same number of bars
    EXPECT_EQ(inline_result.strategy_calls, bars);
    EXPECT_EQ(logging_result.strategy_calls, bars);
    EXPECT_EQ(light_result.strategy_calls, bars);
    EXPECT_EQ(standard_result.strategy_calls, bars);
    EXPECT_EQ(full_result.strategy_calls, bars);
    EXPECT_EQ(extended_result.strategy_calls, bars);
}

TEST(ThreadingCorrectness, AllPresetsComplete)
{
    // Verify each preset can run a backtest to completion without errors
    for (auto preset : {thread_preset::inline_mode,
                        thread_preset::logging_only, thread_preset::light,
                        thread_preset::standard, thread_preset::full,
                        thread_preset::extended})
    {
        EXPECT_NO_THROW(run_preset(preset, 50))
            << "Failed for preset: " << preset_to_string(preset);
    }
}

// With ring_drop_policy::block (backtest default) and the synchronous
// risk-view mark, a threaded preset must report exactly the same results
// as inline mode — comparing strategies on a multicore box must never
// compare scheduling noise.
TEST(ThreadingCorrectness, ThreadedPresetsMatchInlineResults)
{
    auto run_report = [](thread_preset preset) {
        SilenceCout quiet;
        auto dh = make_test_data(200);
        auto ob = std::make_shared<orderbook>();
        auto strat = std::make_shared<CorrectnessStrategy>();

        MarketMaker mm(42);
        mm.add_orders(ob, 100.0, 10);

        engine_config cfg;
        cfg.threading = preset;
        cfg.seed = 42;
        cfg.disable_pinning = true;

        engine eng(dh, ob, strat, std::move(cfg));
        eng.run();
        return eng.get_analytics().generate_report();
    };

    const auto base = run_report(thread_preset::inline_mode);

    for (auto preset : {thread_preset::logging_only, thread_preset::light,
                        thread_preset::standard,
                        thread_preset::full, thread_preset::extended})
    {
        const auto r = run_report(preset);
        EXPECT_DOUBLE_EQ(r.final_equity, base.final_equity)
            << preset_to_string(preset);
        EXPECT_EQ(r.total_fills, base.total_fills) << preset_to_string(preset);
        EXPECT_EQ(r.total_trades, base.total_trades) << preset_to_string(preset);
        EXPECT_DOUBLE_EQ(r.sharpe_ratio, base.sharpe_ratio)
            << preset_to_string(preset);
        EXPECT_DOUBLE_EQ(r.sortino_ratio, base.sortino_ratio)
            << preset_to_string(preset);
        EXPECT_DOUBLE_EQ(r.max_drawdown, base.max_drawdown)
            << preset_to_string(preset);
        EXPECT_EQ(r.equity_curve.size(), base.equity_curve.size())
            << preset_to_string(preset);
    }
}
