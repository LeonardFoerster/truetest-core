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
#include <memory>
#include <limits>
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

// Async-style adapter: submit records the order without filling.
// First poll_fills (inside process_order) returns empty; subsequent polls
// (bar/tick provider drain → handle_engine_fill) release the fill.
class DeferredFillAdapter : public IExecutionAdapter
{
public:
    int submit_count = 0;
    int poll_count = 0;
    std::optional<order_event> held;
    std::optional<double> fill_quantity_override;
    std::optional<double> fill_price_override;

    void submit_order(const order_event& o) override
    {
        ++submit_count;
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
        fill_event f(o.get_earliest_eligible_ts(),
                     o.get_symbol(),
                     o.get_order_id(),
                     o.get_side(),
                     fill_quantity_override.value_or(o.get_quantity()),
                     fill_price_override.value_or(
                         o.get_price() > 0.0 ? o.get_price() : 100.0));
        f.set_recv_ns(o.get_recv_ns());
        if (o.get_recv_ns() > 0)
        {
            const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            f.set_latency_ns(now_ns - o.get_recv_ns());
        }
        f.set_source(fill_source::exchange);
        if (!o.get_strategy_name().empty())
            f.set_strategy_name(o.get_strategy_name());
        out.push_back(std::move(f));
        held.reset();
        return true;
    }

    bool cancel_order(uint64_t) override { return false; }
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
    std::optional<order_event> on_market(const market_event& mkt) override
    {
        ++calls;
        if (fired_) return std::nullopt;
        fired_ = true;
        return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                           order_type::market, order_side::buy,
                           1.0, mkt.get_close());
    }
    void on_fill(const fill_event&, std::uint64_t) override { ++fill_calls; }
    void set_position_open(const std::string&, bool) override {}
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

TEST(TickToTradeSafety, ProviderOverflowFillHaltsBeforeObserverMutation)
{
    silence_cout quiet;
    auto dh = make_bars(5);
    auto provider = std::make_shared<DeferredFillProvider>();
    provider->adapter->fill_quantity_override = std::numeric_limits<double>::max();
    provider->adapter->fill_price_override = 2.0;
    auto strat = std::make_shared<OneShotBuyer>();

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.provider = provider;
    cfg.initial_balance = 100000.0;
    cfg.threading = thread_preset::inline_mode;
    cfg.disable_pinning = true;

    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.set_primary_strategy_name("oneshot");
    eng.run();

    const auto report = eng.get_analytics().generate_report();
    EXPECT_TRUE(eng.is_halted());
    EXPECT_EQ(report.total_fills, 0u);
    EXPECT_DOUBLE_EQ(report.final_equity, 100000.0);
    EXPECT_EQ(strat->fill_calls, 0)
        << "a rejected preflight must not invoke strategy fill observers";
    EXPECT_EQ(eng.get_order_tracker().active_count(), 1u)
        << "preflight must precede the fill tracker status transition";
    EXPECT_EQ(eng.get_order_tracker().get_order_status(1), order_status::pending);
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

    engine eng(dh, ob, strat, std::move(cfg));
    eng.set_primary_strategy_name("l2");

    // First update: strategy emits; default execution_bar_delay parks the order.
    eng.apply_l2_update("TEST", tick_side::bid, 100.0, 50);
    EXPECT_EQ(strat->l2_calls, 1);
    EXPECT_GT(strat->last_event_recv_ns, 0)
        << "apply_l2_update must stamp steady_clock recv_ns on the event";

    // Second update: pending drain submits the parked order (pure L2 stream).
    eng.apply_l2_update("TEST", tick_side::bid, 100.5, 40);

    EXPECT_EQ(provider->adapter->submit_count, 1)
        << "L2 strategy order must drain via pending + process_order → submit";
    EXPECT_GT(provider->adapter->last_order_id, 0u);
    EXPECT_GT(provider->adapter->last_recv_ns, 0)
        << "order must carry recv_ns for tick-to-trade stamping";
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
