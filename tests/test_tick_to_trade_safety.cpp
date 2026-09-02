// Targeted coverage for tick-to-trade safety fixes:
//   - handle_engine_fill on provider async fill drain (post-fill risk halt)
//   - finalize_strategy_route on pause (unlock optimistic gates + drain intents)
//   - L2 strategy dispatch (recv_ns, route)
//   - Latency stamping on LocalBookAdapter fills

#include <gtest/gtest.h>

#include "core/event.h"
#include "data/data_handler.h"
#include "engine/engine.h"
#include "engine/engine_config.h"
#include "execution/execution_adapter.h"
#include "exits/exit_intent.h"
#include "market_maker/market_maker.h"
#include "orderbook/orderbook.h"
#include "providers/provider.h"
#include "strategy/strategy_interface.h"

#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct silence_cout
{
    std::ostringstream sink;
    std::streambuf* orig;
    silence_cout() : sink(), orig(std::cout.rdbuf(sink.rdbuf())) {}
    ~silence_cout() { std::cout.rdbuf(orig); }
};

static auto now_tp() { return std::chrono::system_clock::now(); }

enum class injected_fill_fault
{
    none,
    missing_fill_id,
    missing_venue_execution_id,
    missing_commission_currency,
    missing_cumulative_quantity,
    unknown_order,
    mismatched_symbol,
    mismatched_side,
    zero_quantity,
    nan_quantity,
    infinite_price,
    nan_commission,
    oversized_quantity,
    epoch_zero_timestamp,
    simulated_fee_without_currency,
    simulated_fee_with_currency,
    simulated_rebate_with_currency,
};

// Async-style adapter: submit records the order without filling.
// First poll_fills (inside process_order) returns empty; subsequent polls
// (bar/tick provider drain → handle_engine_fill) release the fill.
class DeferredFillAdapter : public IExecutionAdapter
{
public:
    int submit_count = 0;
    int poll_count = 0;
    std::uint64_t last_order_id = 0;
    double commission = 0.0;
    std::string commission_currency = "USDT";
    injected_fill_fault fill_fault = injected_fill_fault::none;
    std::optional<order_event> held;

    void submit_order(const order_event& o) override
    {
        ++submit_count;
        last_order_id = o.get_order_id();
        held = o;
        // Reset so the process_order poll after this submit is skipped once.
        poll_count = 0;
    }

    bool poll_fills(std::vector<fill_event>& out) override
    {
        ++poll_count;
        if (!held.has_value()) return false;
        // Skip the immediate process_order poll; deliver on later drain.
        if (poll_count < 2) return false;

        const order_event& o = *held;
        auto timestamp = o.get_earliest_eligible_ts();
        std::string symbol = o.get_symbol();
        auto order_id = o.get_order_id();
        auto side = o.get_side();
        double quantity = o.get_quantity();
        double price = o.get_price() > 0.0 ? o.get_price() : 100.0;
        double fill_commission = commission;
        double remaining = 0.0;
        auto fill_id = ++next_fill_id_;
        double cumulative = o.get_quantity();
        auto source = fill_source::exchange;
        auto cumulative_source = fill_cumulative_source::venue_reported;
        bool stamp_venue_execution_id = true;
        bool stamp_commission_currency = true;
        bool stamp_cumulative_quantity = true;

        switch (fill_fault)
        {
        case injected_fill_fault::none: break;
        case injected_fill_fault::missing_fill_id: fill_id = 0; break;
        case injected_fill_fault::missing_venue_execution_id:
            stamp_venue_execution_id = false;
            break;
        case injected_fill_fault::missing_commission_currency:
            stamp_commission_currency = false;
            break;
        case injected_fill_fault::missing_cumulative_quantity:
            stamp_cumulative_quantity = false;
            break;
        case injected_fill_fault::unknown_order: ++order_id; break;
        case injected_fill_fault::mismatched_symbol: symbol = "OTHER"; break;
        case injected_fill_fault::mismatched_side:
            side = side == order_side::buy ? order_side::sell : order_side::buy;
            break;
        case injected_fill_fault::zero_quantity: quantity = 0.0; break;
        case injected_fill_fault::nan_quantity:
            quantity = std::numeric_limits<double>::quiet_NaN();
            break;
        case injected_fill_fault::infinite_price:
            price = std::numeric_limits<double>::infinity();
            break;
        case injected_fill_fault::nan_commission:
            fill_commission = std::numeric_limits<double>::quiet_NaN();
            break;
        case injected_fill_fault::oversized_quantity:
            quantity = o.get_quantity() + 1.0;
            cumulative = quantity;
            break;
        case injected_fill_fault::epoch_zero_timestamp:
            timestamp = {};
            break;
        case injected_fill_fault::simulated_fee_without_currency:
            source = fill_source::simulated;
            cumulative_source = fill_cumulative_source::simulated;
            fill_commission = 1.0;
            stamp_commission_currency = false;
            break;
        case injected_fill_fault::simulated_fee_with_currency:
            source = fill_source::simulated;
            cumulative_source = fill_cumulative_source::simulated;
            fill_commission = 1.0;
            break;
        case injected_fill_fault::simulated_rebate_with_currency:
            source = fill_source::simulated;
            cumulative_source = fill_cumulative_source::simulated;
            fill_commission = -1.0;
            break;
        }

        fill_event f(timestamp, symbol, order_id, side, quantity, price,
                     fill_commission, remaining, fill_id);
        f.set_recv_ns(o.get_recv_ns());
        if (o.get_recv_ns() > 0)
        {
            const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            f.set_latency_ns(now_ns - o.get_recv_ns());
        }
        f.set_source(source);
        if (stamp_venue_execution_id)
        {
            EXPECT_TRUE(f.set_venue_execution_id(
                "deferred-" + std::to_string(f.get_fill_id())));
        }
        if (stamp_commission_currency)
        {
            EXPECT_TRUE(f.set_commission_currency(commission_currency));
        }
        if (stamp_cumulative_quantity)
            f.set_cumulative_filled_qty(
                cumulative, cumulative_source);
        if (!o.get_strategy_name().empty())
            f.set_strategy_name(o.get_strategy_name());
        out.push_back(std::move(f));
        held.reset();
        return true;
    }

    bool cancel_order(uint64_t) override { return false; }

private:
    std::uint64_t next_fill_id_ = 0;
};

class DeferredFillProvider : public IProvider
{
public:
    std::shared_ptr<DeferredFillAdapter> adapter =
        std::make_shared<DeferredFillAdapter>();

    std::string name() const override { return "deferred-fill"; }
    bool has_data_feed() const override { return false; }
    bool has_execution() const override { return true; }
    bool open() override { return true; }
    void close() override {}
    std::shared_ptr<IDataTransport> get_transport() override { return nullptr; }
    std::shared_ptr<IExecutionAdapter> get_execution_adapter() override
    {
        return adapter;
    }
};

// Fires one market buy on the first bar only.
class OneShotBuyer : public IStrategy
{
    bool fired_ = false;
public:
    int calls = 0;
    int fill_calls = 0;
    bool emit_exit_intent = false;
    std::vector<truetest::exits::exit_intent> pending_exit_intents;
    std::optional<order_event> on_market(const market_event& mkt) override
    {
        ++calls;
        if (fired_) return std::nullopt;
        fired_ = true;
        if (emit_exit_intent)
        {
            truetest::exits::exit_intent intent;
            intent.symbol = mkt.get_symbol();
            intent.close_side = order_side::sell;
            intent.qty = 1.0;
            intent.stop_loss = mkt.get_close() * 0.95;
            pending_exit_intents.push_back(std::move(intent));
        }
        return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                           order_type::market, order_side::buy,
                           1.0, mkt.get_close());
    }
    void set_position_open(const std::string&, bool) override {}
    void on_fill(const fill_event&, std::uint64_t) override { ++fill_calls; }
    std::vector<truetest::exits::exit_intent>
    take_pending_exit_intents() override
    {
        auto out = std::move(pending_exit_intents);
        pending_exit_intents.clear();
        return out;
    }
};

// Optimistic gate: locks on emit (like mean-reversion). Engine must unlock
// via set_position_open when route is paused/rejected.
class OptimisticGateStrategy : public IStrategy
{
public:
    bool gate_open = false;
    int emit_count = 0;
    int set_position_calls = 0;
    bool last_set_position_open = true;
    std::vector<truetest::exits::exit_intent> pending;

    std::optional<order_event> on_market(const market_event& mkt) override
    {
        if (gate_open) return std::nullopt;
        gate_open = true;
        ++emit_count;

        truetest::exits::exit_intent ei;
        ei.symbol = mkt.get_symbol();
        ei.close_side = order_side::sell;
        ei.qty = 1.0;
        ei.stop_loss = mkt.get_close() * 0.99;
        pending.push_back(ei);

        return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                           order_type::market, order_side::buy,
                           1.0, mkt.get_close());
    }

    void set_position_open(const std::string&, bool open) override
    {
        ++set_position_calls;
        last_set_position_open = open;
        gate_open = open;
    }

    std::vector<truetest::exits::exit_intent> take_pending_exit_intents() override
    {
        auto out = std::move(pending);
        pending.clear();
        return out;
    }
};

class L2OneShotStrategy : public IStrategy
{
    bool fired_ = false;
public:
    int l2_calls = 0;
    int64_t last_event_recv_ns = 0;

    std::optional<order_event> on_market(const market_event&) override
    {
        return std::nullopt;
    }

    std::optional<order_event> on_l2_update(const l2_update_event& ev) override
    {
        ++l2_calls;
        last_event_recv_ns = ev.get_recv_ns();
        if (fired_) return std::nullopt;
        fired_ = true;
        return order_event(ev.get_timestamp(), ev.get_symbol(),
                           order_type::limit, order_side::buy,
                           1.0, ev.get_price());
    }

    void set_position_open(const std::string&, bool) override {}
};

std::shared_ptr<data_handler> make_bars(int n)
{
    auto dh = std::make_shared<data_handler>();
    double px = 100.0;
    for (int i = 0; i < n; ++i)
    {
        dh->load_into_queue("2024-01-01", "TEST",
                            px, px + 0.5, px - 0.5, px, 1000);
        px += 0.1;
    }
    return dh;
}

} // namespace

// ---------------------------------------------------------------------------
// Provider fill drain → handle_engine_fill → post-fill risk halt
// ---------------------------------------------------------------------------

TEST(TickToTradeSafety, ProviderFill_PostFillRiskHaltsProcessWide)
{
    silence_cout quiet;
    auto dh = make_bars(20);
    auto provider = std::make_shared<DeferredFillProvider>();
    auto strat = std::make_shared<OneShotBuyer>();

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.provider = provider;
    cfg.initial_balance = 100000.0;
    cfg.threading = thread_preset::inline_mode;
    cfg.disable_pinning = true;
    cfg.risk_soft_portfolio_limits = false; // hard post-fill halt under test
    // First fill increments trade_timestamps; check_post_fill sees size >= 1 → halt.
    cfg.risk.max_trades_per_hour = 1;
    cfg.risk.max_drawdown = 1.0;
    cfg.risk.max_loss_per_trade = 1e12;

    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.set_primary_strategy_name("oneshot");
    eng.run();

    EXPECT_GE(provider->adapter->submit_count, 1);
    EXPECT_TRUE(eng.get_halt_flag().load(std::memory_order_acquire))
        << "provider-drained fill must run check_post_fill and raise halt_flag_";
    // Strategy must stop being called after terminal halt.
    EXPECT_LT(strat->calls, 20);
}

TEST(TickToTradeSafety, ProviderFill_RecordsInAnalytics)
{
    silence_cout quiet;
    auto dh = make_bars(5);
    auto provider = std::make_shared<DeferredFillProvider>();
    auto strat = std::make_shared<OneShotBuyer>();

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.provider = provider;
    cfg.initial_balance = 100000.0;
    cfg.threading = thread_preset::inline_mode;
    cfg.disable_pinning = true;
    // No tight risk — fill should complete cleanly.
    cfg.risk.max_trades_per_hour = 0;

    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.set_primary_strategy_name("oneshot");
    eng.run();

    auto report = eng.get_analytics().generate_report();
    EXPECT_GE(provider->adapter->submit_count, 1);
    EXPECT_GE(report.total_fills, 1u)
        << "handle_engine_fill must always call analytics_.on_event on provider fills";
    EXPECT_FALSE(eng.get_halt_flag().load(std::memory_order_acquire));
}

TEST(TickToTradeSafety, C08_ThirdAssetFeeWithoutFxFailsBeforeEconomicMutation)
{
    silence_cout quiet;
    auto dh = std::make_shared<data_handler>();
    for (int i = 0; i < 5; ++i)
    {
        dh->load_into_queue("1704067200000", "BTCUSDT",
                            100.0, 100.5, 99.5, 100.0, 1'000);
    }
    auto provider = std::make_shared<DeferredFillProvider>();
    provider->adapter->commission = 1.0;
    provider->adapter->commission_currency = "BNB";
    auto strat = std::make_shared<OneShotBuyer>();

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.provider = provider;
    cfg.initial_balance = 100'000.0;
    cfg.threading = thread_preset::inline_mode;
    cfg.disable_pinning = true;
    cfg.risk.max_trades_per_hour = 0;

    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.set_primary_strategy_name("oneshot");
    eng.run();

    EXPECT_TRUE(eng.is_halted());
    EXPECT_FALSE(eng.run_succeeded());
    EXPECT_FALSE(eng.get_portfolio().position_open("BTCUSDT"));
    EXPECT_DOUBLE_EQ(eng.get_portfolio().get_cash(), 100'000.0);
    const auto order_id = provider->adapter->last_order_id;
    ASSERT_NE(order_id, 0u);
    const auto* tracked = eng.get_order_tracker().find(order_id);
    ASSERT_NE(tracked, nullptr);
    EXPECT_DOUBLE_EQ(tracked->filled_qty, 0.0);
    EXPECT_DOUBLE_EQ(eng.get_order_tracker().pending_qty(order_id), 1.0);
    EXPECT_EQ(eng.get_exit_manager().armed_count(), 0u);
    const auto report = eng.get_analytics().generate_report();
    EXPECT_EQ(report.total_fills, 0u);
    EXPECT_DOUBLE_EQ(report.total_commission, 0.0);
}

TEST(TickToTradeSafety, C03_InvalidVenueFillIsRejectedBeforeEveryEconomicMutation)
{
    constexpr injected_fill_fault faults[] = {
        injected_fill_fault::missing_fill_id,
        injected_fill_fault::missing_venue_execution_id,
        injected_fill_fault::missing_commission_currency,
        injected_fill_fault::missing_cumulative_quantity,
        injected_fill_fault::unknown_order,
        injected_fill_fault::mismatched_symbol,
        injected_fill_fault::mismatched_side,
        injected_fill_fault::zero_quantity,
        injected_fill_fault::nan_quantity,
        injected_fill_fault::infinite_price,
        injected_fill_fault::nan_commission,
        injected_fill_fault::oversized_quantity,
        injected_fill_fault::epoch_zero_timestamp,
    };

    for (const auto fault : faults)
    {
        SCOPED_TRACE(static_cast<int>(fault));
        silence_cout quiet;
        auto dh = make_bars(5);
        auto provider = std::make_shared<DeferredFillProvider>();
        provider->adapter->fill_fault = fault;
        auto strat = std::make_shared<OneShotBuyer>();
        strat->emit_exit_intent = true;

        engine_config cfg;
        cfg.mode = engine_mode::backtest;
        cfg.provider = provider;
        cfg.initial_balance = 100'000.0;
        cfg.threading = thread_preset::inline_mode;
        cfg.disable_pinning = true;
        cfg.risk.max_trades_per_hour = 0;

        engine eng(dh, nullptr, strat, std::move(cfg));
        eng.set_primary_strategy_name("oneshot");
        eng.run();

        EXPECT_TRUE(eng.is_halted());
        EXPECT_FALSE(eng.run_succeeded());
        EXPECT_FALSE(eng.get_portfolio().position_open("TEST"));
        EXPECT_TRUE(eng.get_portfolio().get_positions().empty());
        EXPECT_TRUE(eng.get_portfolio().get_lots().empty());
        EXPECT_DOUBLE_EQ(eng.get_portfolio().get_cash(), 100'000.0);

        const auto order_id = provider->adapter->last_order_id;
        ASSERT_NE(order_id, 0U);
        const auto* tracked = eng.get_order_tracker().find(order_id);
        ASSERT_NE(tracked, nullptr);
        EXPECT_DOUBLE_EQ(tracked->filled_qty, 0.0);
        EXPECT_DOUBLE_EQ(eng.get_order_tracker().pending_qty(order_id), 1.0);
        EXPECT_EQ(eng.get_exit_manager().pending_count(), 1U);
        EXPECT_EQ(eng.get_exit_manager().armed_count(), 0U);
        EXPECT_EQ(eng.get_exit_manager().counters().pending_registered, 1U);
        EXPECT_EQ(eng.get_exit_manager().counters().armed, 0U);
        EXPECT_EQ(strat->fill_calls, 0);

        const auto report = eng.get_analytics().generate_report();
        EXPECT_EQ(report.total_fills, 0U);
        EXPECT_DOUBLE_EQ(report.total_commission, 0.0);
        EXPECT_TRUE(report.trades.empty());
    }
}

TEST(TickToTradeSafety, C03_SimulatedFeeWithoutCurrencyFailsClosed)
{
    silence_cout quiet;
    auto dh = make_bars(5);
    auto provider = std::make_shared<DeferredFillProvider>();
    provider->adapter->fill_fault =
        injected_fill_fault::simulated_fee_without_currency;
    auto strat = std::make_shared<OneShotBuyer>();
    strat->emit_exit_intent = true;

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.provider = provider;
    cfg.initial_balance = 100'000.0;
    cfg.threading = thread_preset::inline_mode;
    cfg.disable_pinning = true;

    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.set_primary_strategy_name("oneshot");
    eng.run();

    EXPECT_TRUE(eng.is_halted());
    EXPECT_FALSE(eng.run_succeeded());
    EXPECT_FALSE(eng.get_portfolio().position_open("TEST"));
    EXPECT_DOUBLE_EQ(eng.get_portfolio().get_cash(), 100'000.0);
    const auto order_id = provider->adapter->last_order_id;
    ASSERT_NE(order_id, 0U);
    const auto* tracked = eng.get_order_tracker().find(order_id);
    ASSERT_NE(tracked, nullptr);
    EXPECT_DOUBLE_EQ(tracked->filled_qty, 0.0);
    EXPECT_DOUBLE_EQ(eng.get_order_tracker().pending_qty(order_id), 1.0);
    EXPECT_EQ(eng.get_exit_manager().pending_count(), 1U);
    EXPECT_EQ(eng.get_exit_manager().armed_count(), 0U);
    EXPECT_EQ(strat->fill_calls, 0);
    const auto report = eng.get_analytics().generate_report();
    EXPECT_EQ(report.total_fills, 0U);
    EXPECT_DOUBLE_EQ(report.total_commission, 0.0);
    EXPECT_TRUE(report.trades.empty());
}

TEST(TickToTradeSafety, C03_ValidSimulatedFeeWithCurrencyCommitsExactlyOnce)
{
    silence_cout quiet;
    auto dh = make_bars(5);
    auto provider = std::make_shared<DeferredFillProvider>();
    provider->adapter->fill_fault =
        injected_fill_fault::simulated_fee_with_currency;
    auto strat = std::make_shared<OneShotBuyer>();

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.provider = provider;
    cfg.initial_balance = 100'000.0;
    cfg.threading = thread_preset::inline_mode;
    cfg.disable_pinning = true;

    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.set_primary_strategy_name("oneshot");
    eng.run();

    EXPECT_FALSE(eng.is_halted());
    EXPECT_TRUE(eng.run_succeeded());
    EXPECT_TRUE(eng.get_portfolio().position_open("TEST"));
    EXPECT_DOUBLE_EQ(eng.get_portfolio().get_cash(), 99'899.0);
    const auto order_id = provider->adapter->last_order_id;
    ASSERT_NE(order_id, 0U);
    const auto* tracked = eng.get_order_tracker().find(order_id);
    ASSERT_NE(tracked, nullptr);
    EXPECT_DOUBLE_EQ(tracked->filled_qty, 1.0);
    EXPECT_DOUBLE_EQ(eng.get_order_tracker().pending_qty(order_id), 0.0);
    EXPECT_EQ(strat->fill_calls, 1);
    const auto report = eng.get_analytics().generate_report();
    EXPECT_EQ(report.total_fills, 1U);
    EXPECT_DOUBLE_EQ(report.total_commission, 1.0);
    ASSERT_EQ(report.trades.size(), 1U);
}

TEST(TickToTradeSafety,
     C03_ValidSimulatedMakerRebateWithCurrencyCommitsExactlyOnce)
{
    silence_cout quiet;
    auto dh = make_bars(5);
    auto provider = std::make_shared<DeferredFillProvider>();
    provider->adapter->fill_fault =
        injected_fill_fault::simulated_rebate_with_currency;
    auto strat = std::make_shared<OneShotBuyer>();

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.provider = provider;
    cfg.initial_balance = 100'000.0;
    cfg.threading = thread_preset::inline_mode;
    cfg.disable_pinning = true;

    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.set_primary_strategy_name("oneshot");
    eng.run();

    EXPECT_FALSE(eng.is_halted());
    EXPECT_TRUE(eng.run_succeeded());
    EXPECT_TRUE(eng.get_portfolio().position_open("TEST"));
    EXPECT_DOUBLE_EQ(eng.get_portfolio().get_cash(), 99'901.0);
    const auto order_id = provider->adapter->last_order_id;
    ASSERT_NE(order_id, 0U);
    const auto* tracked = eng.get_order_tracker().find(order_id);
    ASSERT_NE(tracked, nullptr);
    EXPECT_DOUBLE_EQ(tracked->filled_qty, 1.0);
    EXPECT_DOUBLE_EQ(eng.get_order_tracker().pending_qty(order_id), 0.0);
    EXPECT_EQ(strat->fill_calls, 1);
    const auto report = eng.get_analytics().generate_report();
    EXPECT_EQ(report.total_fills, 1U);
    EXPECT_DOUBLE_EQ(report.total_commission, -1.0);
    ASSERT_EQ(report.trades.size(), 1U);
    EXPECT_DOUBLE_EQ(report.trades.front().commission, -1.0);
    EXPECT_EQ(report.trades.front().commission_currency, "USDT");
    EXPECT_DOUBLE_EQ(report.unrealized_pnl, 1.0);
    EXPECT_DOUBLE_EQ(report.final_equity, 100'001.0);
    EXPECT_DOUBLE_EQ(report.reconciliation_residual, 0.0);
    EXPECT_TRUE(report.accounting_reconciled);
    EXPECT_TRUE(report.valuation_complete);
}

TEST(TickToTradeSafety, C03_ShadowExchangeFillCannotBypassCanonicalAdmission)
{
    silence_cout quiet;
    auto dh = make_bars(5);
    auto provider = std::make_shared<DeferredFillProvider>();
    provider->adapter->fill_fault = injected_fill_fault::mismatched_symbol;
    auto strat = std::make_shared<OneShotBuyer>();

    engine_config cfg;
    cfg.mode = engine_mode::shadow;
    cfg.provider = provider;
    cfg.initial_balance = 100'000.0;
    cfg.threading = thread_preset::inline_mode;
    cfg.disable_pinning = true;
    cfg.risk.max_trades_per_hour = 0;

    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.set_primary_strategy_name("oneshot");
    eng.run();

    EXPECT_TRUE(eng.is_halted())
        << "an invalid exchange-side shadow fill requires reconciliation";
    EXPECT_FALSE(eng.run_succeeded());
    const auto* exchange_portfolio = eng.get_exchange_portfolio();
    ASSERT_NE(exchange_portfolio, nullptr);
    EXPECT_TRUE(exchange_portfolio->get_positions().empty());
    EXPECT_TRUE(exchange_portfolio->get_lots().empty());
    EXPECT_DOUBLE_EQ(exchange_portfolio->get_cash(), 100'000.0);
    const auto* exchange_analytics = eng.get_exchange_analytics();
    ASSERT_NE(exchange_analytics, nullptr);
    const auto report = exchange_analytics->generate_report();
    EXPECT_EQ(report.total_fills, 0U);
    EXPECT_TRUE(report.trades.empty());
}

// ---------------------------------------------------------------------------
// finalize_strategy_route: pause drops order, unlocks optimistic gate
// ---------------------------------------------------------------------------

TEST(TickToTradeSafety, Pause_UnlocksOptimisticGateAndDrainsIntents)
{
    silence_cout quiet;
    auto dh = make_bars(8);
    auto strat = std::make_shared<OptimisticGateStrategy>();

    // Seed book so unpaused runs would fill (not needed while paused).
    auto ob = std::make_shared<orderbook>();
    MarketMaker mm(3);
    mm.add_orders(ob, 100.0, 50);

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.initial_balance = 100000.0;
    cfg.threading = thread_preset::inline_mode;
    cfg.disable_pinning = true;

    engine eng(dh, ob, strat, std::move(cfg));
    eng.set_primary_strategy_name("opt");
    eng.set_pause_all(true);
    eng.run();

    // Strategy emitted at least once (strategies still run under pause).
    EXPECT_GE(strat->emit_count, 1);
    // finalize_strategy_route must resync gate via set_position_open(false).
    EXPECT_FALSE(strat->gate_open)
        << "paused route must unlock optimistic gate so re-entry is possible later";
    EXPECT_GE(strat->set_position_calls, 1);
    EXPECT_FALSE(strat->last_set_position_open);
    // Intents drained, not left pending for a later unrelated entry.
    EXPECT_TRUE(strat->pending.empty());
    auto report = eng.get_analytics().generate_report();
    EXPECT_EQ(report.total_orders, 0u);
    EXPECT_EQ(report.total_fills, 0u);
}

// ---------------------------------------------------------------------------
// L2 dispatch: real recv_ns + strategy route
// ---------------------------------------------------------------------------

// Counts submits without filling (isolates L2 → route → process_order).
class CountingSubmitAdapter : public IExecutionAdapter
{
public:
    int submit_count = 0;
    void set_mid_price(double mid) override { last_mid = mid; }
    void submit_order(const order_event& o) override
    {
        ++submit_count;
        last_recv_ns = o.get_recv_ns();
        last_order_id = o.get_order_id();
    }
    bool poll_fills(std::vector<fill_event>&) override { return false; }
    bool cancel_order(uint64_t) override { return false; }
    int64_t last_recv_ns = 0;
    uint64_t last_order_id = 0;
    double last_mid = 0.0;
};

class CountingSubmitProvider : public IProvider
{
public:
    std::shared_ptr<CountingSubmitAdapter> adapter =
        std::make_shared<CountingSubmitAdapter>();
    std::string name() const override { return "count-submit"; }
    bool has_data_feed() const override { return false; }
    bool has_execution() const override { return true; }
    bool open() override { return true; }
    void close() override {}
    std::shared_ptr<IDataTransport> get_transport() override { return nullptr; }
    std::shared_ptr<IExecutionAdapter> get_execution_adapter() override
    {
        return adapter;
    }
};

TEST(TickToTradeSafety, L2Dispatch_StampsRecvNsAndRoutesOrder)
{
    silence_cout quiet;
    auto dh = make_bars(2); // unused for L2 path
    auto strat = std::make_shared<L2OneShotStrategy>();
    auto provider = std::make_shared<CountingSubmitProvider>();
    auto ob = std::make_shared<orderbook>();

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.provider = provider;
    cfg.initial_balance = 100000.0;
    cfg.threading = thread_preset::inline_mode;
    cfg.disable_pinning = true;
    cfg.execution_bar_delay = 1;

    engine eng(dh, ob, strat, std::move(cfg));
    eng.set_primary_strategy_name("l2");

    const auto base_ts = std::chrono::system_clock::time_point{
        std::chrono::seconds{1'700'000'000}};

    // First TEST observation: strategy emits; default delay=1 parks the order.
    eng.apply_l2_update("TEST", tick_side::bid, 100.0, 50, base_ts);
    EXPECT_EQ(strat->l2_calls, 1);
    EXPECT_GT(strat->last_event_recv_ns, 0)
        << "apply_l2_update must stamp steady_clock recv_ns on the event";
    EXPECT_EQ(provider->adapter->submit_count, 0);

    // An unrelated symbol is not a future TEST observation and must not
    // release the parked order.
    eng.apply_l2_update("OTHER", tick_side::ask, 200.0, 25,
                        base_ts + std::chrono::seconds{1});
    EXPECT_EQ(provider->adapter->submit_count, 0);

    // The next same-symbol observation releases the order (pure L2 stream).
    eng.apply_l2_update("TEST", tick_side::bid, 100.5, 40,
                        base_ts + std::chrono::seconds{2});

    EXPECT_EQ(provider->adapter->submit_count, 1)
        << "L2 strategy order must drain on the next same-symbol observation";
    EXPECT_GT(provider->adapter->last_order_id, 0u);
    EXPECT_GT(provider->adapter->last_recv_ns, 0)
        << "order must carry recv_ns for tick-to-trade stamping";
    EXPECT_DOUBLE_EQ(provider->adapter->last_mid, 100.5)
        << "pure L2 execution/risk must use the releasing symbol's current mark";
}

TEST(TickToTradeSafety, L2SnapshotIsSameSymbolObservationForDelayedOrder)
{
    silence_cout quiet;
    auto strat = std::make_shared<L2OneShotStrategy>();
    auto provider = std::make_shared<CountingSubmitProvider>();

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.provider = provider;
    cfg.threading = thread_preset::inline_mode;
    cfg.disable_pinning = true;
    cfg.execution_bar_delay = 1;
    engine eng(make_bars(1), std::make_shared<orderbook>(), strat,
               std::move(cfg));

    const auto base_ts = std::chrono::system_clock::time_point{
        std::chrono::seconds{1'700'000'000}};
    eng.apply_l2_update("TEST", tick_side::bid, 100.0, 50, base_ts);
    ASSERT_EQ(provider->adapter->submit_count, 0);

    eng.apply_l2_snapshot("OTHER", {{200.0, 10}}, {{201.0, 10}},
                          base_ts + std::chrono::seconds{1});
    EXPECT_EQ(provider->adapter->submit_count, 0);

    eng.apply_l2_snapshot("TEST", {{100.0, 10}}, {{101.0, 10}},
                          base_ts + std::chrono::seconds{2});
    EXPECT_EQ(provider->adapter->submit_count, 1);
    EXPECT_DOUBLE_EQ(provider->adapter->last_mid, 100.5);
}

// ---------------------------------------------------------------------------
// After terminal halt, route_order refuses further submits (L2 multi-strategy)
// ---------------------------------------------------------------------------

TEST(TickToTradeSafety, HaltedEngine_L2AdditionalStrategyDoesNotSubmit)
{
    silence_cout quiet;
    auto dh = make_bars(2);
    auto primary = std::make_shared<L2OneShotStrategy>();
    auto secondary = std::make_shared<L2OneShotStrategy>();
    auto ob = std::make_shared<orderbook>();

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.threading = thread_preset::inline_mode;
    cfg.disable_pinning = true;

    engine eng(dh, ob, primary, std::move(cfg));
    eng.set_primary_strategy_name("primary");
    eng.add_strategy(secondary, "secondary");

    eng.trigger_halt("pre-halt for L2 refuse test");
    ASSERT_TRUE(eng.get_halt_flag().load(std::memory_order_acquire));

    eng.apply_l2_update("TEST", tick_side::ask, 101.0, 40);

    // Outer halt gate: neither strategy should be invoked for order routing.
    EXPECT_EQ(primary->l2_calls, 0);
    EXPECT_EQ(secondary->l2_calls, 0);
    auto report = eng.get_analytics().generate_report();
    EXPECT_EQ(report.total_orders, 0u);
}

// ---------------------------------------------------------------------------
// Latency stamping (LocalBookAdapter)
// ---------------------------------------------------------------------------

// Relentless buyer: emits every bar (until halt stops strategy routing).
class EveryBarBuyer : public IStrategy
{
public:
    int calls = 0;
    std::optional<order_event> on_market(const market_event& mkt) override
    {
        ++calls;
        return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                           order_type::market, order_side::buy,
                           1.0, mkt.get_close());
    }
    void set_position_open(const std::string&, bool) override {}
};

// Immediate fill on submit (process_order path → handle_engine_fill → post-fill risk).
class ImmediateFillAdapter : public IExecutionAdapter
{
public:
    int submit_count = 0;
    void submit_order(const order_event& o) override
    {
        ++submit_count;
        fill_event f(o.get_earliest_eligible_ts(), o.get_symbol(), o.get_order_id(),
                     o.get_side(), o.get_quantity(),
                     o.get_price() > 0.0 ? o.get_price() : 100.0);
        f.set_source(fill_source::exchange);
        if (!o.get_strategy_name().empty())
            f.set_strategy_name(o.get_strategy_name());
        queue_.push_back(std::move(f));
    }
    bool poll_fills(std::vector<fill_event>& out) override
    {
        if (queue_.empty()) return false;
        out.insert(out.end(),
                   std::make_move_iterator(queue_.begin()),
                   std::make_move_iterator(queue_.end()));
        queue_.clear();
        return true;
    }
    bool cancel_order(uint64_t) override { return false; }
private:
    std::vector<fill_event> queue_;
};

class ImmediateFillProvider : public IProvider
{
public:
    std::shared_ptr<ImmediateFillAdapter> adapter =
        std::make_shared<ImmediateFillAdapter>();
    std::string name() const override { return "immediate-fill"; }
    bool has_data_feed() const override { return false; }
    bool has_execution() const override { return true; }
    bool open() override { return true; }
    void close() override {}
    std::shared_ptr<IDataTransport> get_transport() override { return nullptr; }
    std::shared_ptr<IExecutionAdapter> get_execution_adapter() override
    {
        return adapter;
    }
};

// S3: after process-wide halt from post-fill risk, no further submits.
TEST(TickToTradeSafety, ProcessWideHalt_RefusesFurtherSubmits)
{
    silence_cout quiet;
    auto dh = make_bars(20);
    auto provider = std::make_shared<ImmediateFillProvider>();
    auto strat = std::make_shared<EveryBarBuyer>();

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.provider = provider;
    cfg.initial_balance = 100000.0;
    cfg.threading = thread_preset::inline_mode;
    cfg.disable_pinning = true;
    cfg.execution_bar_delay = 0;  // submit immediately on emit
    cfg.risk_soft_portfolio_limits = false; // hard post-fill halt under test
    // First fill trips post-fill max_trades_per_hour → trigger_halt.
    cfg.risk.max_trades_per_hour = 1;
    cfg.risk.max_drawdown = 1.0;
    cfg.risk.max_loss_per_trade = 1e12;

    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.set_primary_strategy_name("every");
    eng.run();

    EXPECT_TRUE(eng.get_halt_flag().load(std::memory_order_acquire));
    // Exactly one submit (the fill that tripped halt); further bars must not submit.
    EXPECT_EQ(provider->adapter->submit_count, 1)
        << "after halt_flag_, process_order/route_order must refuse further submits";
    EXPECT_LT(strat->calls, 20)
        << "strategy loop should stop once process-wide halt is raised";
}

TEST(TickToTradeSafety, LocalBookAdapter_StampsLatencyFromRecvNs)
{
    auto ob = std::make_shared<orderbook>();
    ob->add_order(std::make_shared<order>(
        ob_order_type::good_till_cancel, 9001, side::sell,
        Price::from_double(100.0), 100));

    LocalBookAdapter adapter(ob, nullptr, nullptr);
    adapter.set_mid_price(100.0);

    order_event o(now_tp(), "TEST", order_type::limit, order_side::buy,
                  1.0, 100.0);
    o.set_order_id(1);
    o.set_earliest_eligible_ts(now_tp());
    const int64_t recv = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() - 50'000;
    o.set_recv_ns(recv);

    adapter.submit_order(o);
    std::vector<fill_event> fills;
    ASSERT_TRUE(adapter.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].get_recv_ns(), recv);
    EXPECT_GT(fills[0].get_latency_ns(), 0);
}
