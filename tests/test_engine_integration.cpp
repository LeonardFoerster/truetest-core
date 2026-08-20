// O1: End-to-end integration tests for the engine pipeline.
// These exercise the full data -> strategy -> orderbook -> fill -> portfolio
// chain - complementing the per-component unit tests in the rest of the
// suite. Scenarios covered:
//   * Deterministic replay: two runs with the same seed + inputs produce
//     byte-identical portfolio state.
//   * Order -> fill -> portfolio update: a forced order produces a fill event
//     that is reflected in both the analytics report and the portfolio.
//   * Risk halt: a tight drawdown limit causes the engine to stop early.
// The tests construct `engine_config` directly (no CLI parsing) and use small
// in-memory data_handlers so the suite stays fast.

#include <gtest/gtest.h>

#include "engine/engine.h"
#include "engine/engine_config.h"
#include "data/data_handler.h"
#include "execution/portfolio.h"
#include "exits/default_exit_policy.h"
#include "market_maker/market_maker.h"
#include "orderbook/orderbook.h"
#include "strategy/sma/sma_strategy.h"
#include "strategy/strategy_interface.h"

#include <chrono>
#include <cmath>
#include <memory>
#include <sstream>

namespace {

// Silence std::cout during runs so the test log stays readable.
struct silence_cout
{
    std::ostringstream sink;
    std::streambuf* orig;
    silence_cout() : sink(), orig(std::cout.rdbuf(sink.rdbuf())) {}
    ~silence_cout() { std::cout.rdbuf(orig); }
};

// Build a synthetic OHLCV dataset with a small periodic oscillation around a
// drifting baseline. The waveform is enough to repeatedly trigger an SMA
// strategy.
std::shared_ptr<data_handler> make_oscillating_bars(int n)
{
    auto dh = std::make_shared<data_handler>();
    for (int i = 0; i < n; ++i)
    {
        double base = 100.0 + 0.05 * i;
        double osc  = 2.0 * std::sin(i * 0.35);
        double close = base + osc;
        double open  = base;
        double high  = std::max(open, close) + 0.5;
        double low   = std::min(open, close) - 0.5;
        dh->load_into_queue("2024-01-01", "TEST", open, high, low, close, 1000);
    }
    return dh;
}

// Forcing strategy: emits one market BUY on bar 2 (so the book is populated)
// and one market SELL on bar 4. Tracks how many times it was invoked.
class forcing_strategy : public IStrategy
{
public:
    std::optional<order_event> on_market(const market_event& mkt) override
    {
        ++calls_;
        if (calls_ == 3 && !position_open_)
        {
            return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                               order_type::market, order_side::buy,
                               1.0, mkt.get_close());
        }
        if (calls_ == 5 && position_open_)
        {
            return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                               order_type::market, order_side::sell,
                               1.0, mkt.get_close());
        }
        return std::nullopt;
    }
    void on_fill(const fill_event&, std::uint64_t) override { ++fills_seen_; }
    void set_position_open(const std::string&, bool open) override { position_open_ = open; }

    int calls() const { return calls_; }
    int fills_seen() const { return fills_seen_; }

private:
    int  calls_        = 0;
    int  fills_seen_   = 0;
    bool position_open_ = false;
};

// Strategy that buys on every bar it's asked - used to drive a drawdown so the
// risk manager trips.
class relentless_buyer : public IStrategy
{
public:
    explicit relentless_buyer(double qty) : qty_(qty) {}

    std::optional<order_event> on_market(const market_event& mkt) override
    {
        ++calls_;
        return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                           order_type::market, order_side::buy,
                           qty_, mkt.get_close());
    }
    void set_position_open(const std::string&, bool) override {}

    int calls() const { return calls_; }

private:
    double qty_ = 1.0;
    int    calls_ = 0;
};

}


// ---------------------------------------------------------------------------
// Full pipeline: CSV-style data -> SMA strategy -> deterministic end state
// ---------------------------------------------------------------------------

TEST(EngineIntegration, SmaPipelineIsDeterministicWithFixedSeed)
{
    silence_cout quiet;

    auto run_once = [](uint64_t seed) {
        auto dh   = make_oscillating_bars(120);
        auto ob   = std::make_shared<orderbook>();
        auto strat = std::make_shared<sma_strategy>(10);

        MarketMaker mm(static_cast<unsigned>(seed + 1));
        mm.add_orders(ob, 100.0, 20);

        engine_config cfg;
        cfg.initial_balance = 10000.0;
        cfg.seed            = seed;
        cfg.threading       = thread_preset::inline_mode;
        cfg.disable_pinning = true;

        engine eng(dh, ob, strat, cfg);
        eng.run();

        const auto& a = eng.get_analytics();
        auto rep = a.generate_report();
        return rep;
    };

    auto a = run_once(424242);
    auto b = run_once(424242);

    EXPECT_DOUBLE_EQ(a.final_equity,       b.final_equity);
    EXPECT_DOUBLE_EQ(a.cumulative_return,  b.cumulative_return);
    EXPECT_EQ      (a.total_fills,         b.total_fills);
    EXPECT_EQ      (a.total_trades,        b.total_trades);
    EXPECT_DOUBLE_EQ(a.max_drawdown,       b.max_drawdown);
}


// ---------------------------------------------------------------------------
// Order -> fill -> portfolio update
// ---------------------------------------------------------------------------

TEST(EngineIntegration, OrderProducesFillAndUpdatesAnalytics)
{
    silence_cout quiet;

    auto dh = make_oscillating_bars(10);
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<forcing_strategy>();

    MarketMaker mm(7);
    mm.add_orders(ob, 100.0, 40);  // Plenty of depth so market orders fill.

    engine_config cfg;
    cfg.initial_balance = 10000.0;
    cfg.seed            = 7;
    cfg.threading       = thread_preset::inline_mode;
    cfg.disable_pinning = true;

    engine eng(dh, ob, strat, cfg);
    eng.run();

    // Strategy saw every bar.
    EXPECT_EQ(strat->calls(), 10);

    // At least the buy was submitted; typically both buy and sell fill.
    auto rep = eng.get_analytics().generate_report();
    EXPECT_GE(rep.total_orders, 1u);
    EXPECT_GE(rep.total_fills,  1u);

    // Order tracker observed at least one order.
    const auto& tracker = eng.get_order_tracker();
    auto open_ids = tracker.get_open_orders();
    // At test end, no orders should still be in a pending/open state.
    EXPECT_TRUE(open_ids.empty());
}

// corr-1 / FR-08: when set_primary_strategy_name is omitted (MC, C API, many
// tests), order_meta strategy_name stays empty. dispatch_fill_to_strategy must
// still deliver fills to the primary strategy so on_fill can reconcile qty.
TEST(EngineIntegration, FillDispatchedToPrimaryWhenStrategyNameUnset)
{
    silence_cout quiet;

    auto dh = make_oscillating_bars(10);
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<forcing_strategy>();

    MarketMaker mm(7);
    mm.add_orders(ob, 100.0, 40);

    engine_config cfg;
    cfg.initial_balance = 10000.0;
    cfg.seed            = 7;
    cfg.threading       = thread_preset::inline_mode;
    cfg.disable_pinning = true;

    engine eng(dh, ob, strat, cfg);
    // Intentionally omit set_primary_strategy_name — regression lock for corr-1.
    eng.run();

    auto rep = eng.get_analytics().generate_report();
    ASSERT_GE(rep.total_fills, 1u);
    EXPECT_EQ(strat->fills_seen(), static_cast<int>(rep.total_fills))
        << "primary strategy on_fill must run even when strategy_name is empty";
}


// ---------------------------------------------------------------------------
// Soft portfolio risk (backtest default): DD rejects new risk but continues
// ---------------------------------------------------------------------------

namespace {

// Shared declining-book setup for soft vs hard portfolio risk.
engine_config make_drawdown_risk_cfg(bool soft)
{
    engine_config cfg;
    cfg.initial_balance = 10000.0;
    cfg.seed            = 3;
    cfg.threading       = thread_preset::inline_mode;
    cfg.disable_pinning = true;
    cfg.risk_soft_portfolio_limits = soft;
    // Isolate portfolio DD: no platform SL/TP that would flatten early.
    cfg.exit_defaults.mode   = truetest::exits::exit_policy_mode::strategy_only;
    cfg.exit_defaults.sl_pct = 0.0;
    cfg.exit_defaults.tp_pct = 0.0;
    cfg.risk.max_drawdown       = 0.05;   // 5% drawdown
    cfg.risk.max_loss_per_trade = 1e9;
    return cfg;
}

std::shared_ptr<data_handler> make_declining_bars(int n)
{
    auto dh = std::make_shared<data_handler>();
    double price = 100.0;
    for (int i = 0; i < n; ++i)
    {
        double close = price * (1.0 - 0.01);
        dh->load_into_queue("2024-01-01", "TEST",
                            price, price + 0.1, close - 0.1, close, 1000);
        price = close;
    }
    return dh;
}

} // namespace

TEST(EngineIntegration, RiskDrawdownSoftContinuesRun)
{
    silence_cout quiet;

    auto dh = make_declining_bars(500);
    auto ob = std::make_shared<orderbook>();
    // Large size so inventory DD breaches 5% quickly without platform SL.
    auto strat = std::make_shared<relentless_buyer>(50.0);

    MarketMaker mm(3);
    mm.add_orders(ob, 100.0, 500);

    engine eng(dh, ob, strat, make_drawdown_risk_cfg(/*soft=*/true));
    eng.run();

    // Soft mode: strategy still sees every bar; no process-wide halt.
    EXPECT_EQ(strat->calls(), 500);
    EXPECT_FALSE(eng.get_halt_flag().load(std::memory_order_acquire));
    const auto rep = eng.get_analytics().generate_report();
    EXPECT_GT(rep.max_drawdown, 5.0);

    // Reject-only proof: risk must have rejected new risk after DD (not pass-through).
    // Without soft→reject mapping this would stay 0 while the run still "continues".
    EXPECT_GT(eng.total_audit_rejections(), 0u);
    // Fills must stop well short of one-per-bar (500 attempts); residual inventory
    // from early fills is fine, but unbounded new risk is not.
    EXPECT_LT(rep.total_fills, static_cast<std::size_t>(strat->calls()) / 2);
}

// Hard stop: same setup with soft limits disabled → terminal halt.
TEST(EngineIntegration, RiskDrawdownHardStopHaltsEngine)
{
    silence_cout quiet;

    auto dh = make_declining_bars(500);
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<relentless_buyer>(50.0);

    MarketMaker mm(3);
    mm.add_orders(ob, 100.0, 500);

    engine eng(dh, ob, strat, make_drawdown_risk_cfg(/*soft=*/false));
    eng.run();

    EXPECT_LT(strat->calls(), 500);
    EXPECT_TRUE(eng.get_halt_flag().load(std::memory_order_acquire));
}

// Pure wiring lock for main.inc resolve_risk_soft_portfolio_limits (no process spawn).
TEST(EngineConfig, RiskSoftPortfolioLimitsResolveByMode)
{
    EXPECT_TRUE(resolve_risk_soft_portfolio_limits(false, "backtest"));
    EXPECT_TRUE(resolve_risk_soft_portfolio_limits(false, ""));
    EXPECT_TRUE(resolve_risk_soft_portfolio_limits(false, "research"));

    EXPECT_FALSE(resolve_risk_soft_portfolio_limits(false, "shadow"));
    EXPECT_FALSE(resolve_risk_soft_portfolio_limits(false, "live"));
    EXPECT_FALSE(resolve_risk_soft_portfolio_limits(true, "backtest"));
    EXPECT_FALSE(resolve_risk_soft_portfolio_limits(true, "shadow"));
    EXPECT_FALSE(resolve_risk_soft_portfolio_limits(true, ""));

    // Explicit override flags in backtest vs live/shadow
    EXPECT_TRUE(resolve_risk_soft_portfolio_limits(false, "backtest", true));
    EXPECT_FALSE(resolve_risk_soft_portfolio_limits(false, "backtest", false));
    EXPECT_FALSE(resolve_risk_soft_portfolio_limits(false, "live", true)); // live mode always fails closed
    EXPECT_FALSE(resolve_risk_soft_portfolio_limits(false, "shadow", true)); // shadow mode always fails closed
}

// Daily loss breach in backtest mode must not halt the engine event loop.
TEST(EngineIntegration, RiskDailyLossSoftContinuesRun)
{
    silence_cout quiet;

    auto dh = make_declining_bars(500);
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<relentless_buyer>(50.0);

    MarketMaker mm(3);
    mm.add_orders(ob, 100.0, 500);

    engine_config cfg;
    cfg.initial_balance = 10000.0;
    cfg.seed            = 3;
    cfg.threading       = thread_preset::inline_mode;
    cfg.disable_pinning = true;
    cfg.risk_soft_portfolio_limits = true;
    cfg.exit_defaults.mode   = truetest::exits::exit_policy_mode::strategy_only;
    cfg.exit_defaults.sl_pct = 0.0;
    cfg.exit_defaults.tp_pct = 0.0;
    cfg.risk.max_daily_loss     = 50.0; // tight $50 daily loss limit
    cfg.risk.max_loss_per_trade = 1e9;
    cfg.risk.max_drawdown       = 1.0;

    engine eng(dh, ob, strat, std::move(cfg));
    eng.run();

    EXPECT_EQ(strat->calls(), 500);
    EXPECT_FALSE(eng.get_halt_flag().load(std::memory_order_acquire));
    EXPECT_GT(eng.total_audit_rejections(), 0u);
}

// soft-post-mode-gate: even if risk_soft_portfolio_limits is left true,
// shadow mode must hard-halt on post-fill portfolio risk (fail-closed).
TEST(EngineIntegration, SoftFlagIgnoredInShadowMode_HardHalts)
{
    silence_cout quiet;

    auto dh = make_declining_bars(500);
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<relentless_buyer>(50.0);

    MarketMaker mm(3);
    mm.add_orders(ob, 100.0, 500);

    engine_config cfg = make_drawdown_risk_cfg(/*soft=*/true);
    cfg.mode = engine_mode::shadow; // ctor must force soft off for non-backtest
    engine eng(dh, ob, strat, std::move(cfg));
    eng.run();

    EXPECT_TRUE(eng.get_halt_flag().load(std::memory_order_acquire))
        << "shadow must not fail-open on soft portfolio flag";
}
