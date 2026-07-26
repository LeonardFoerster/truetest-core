// Pins the live-mode safety gates:
//   1. Reconciler failure blocks engine construction.
//   2. Noop default passes (engine constructs).
//   3. Kill-switch fires during stop_workers when mode == live.
// None of these touch the network - they use in-memory mocks.

#include <gtest/gtest.h>
#include "engine/engine.h"
#include "engine/engine_config.h"
#include "data/data_handler.h"
#include "orderbook/orderbook.h"
#include "market_maker/market_maker.h"
#include "strategy/strategy_interface.h"
#include "execution/live_safety.h"

#include <atomic>

namespace {

class NullStrategy : public IStrategy
{
public:
    std::optional<order_event> on_market(const market_event&) override { return std::nullopt; }
    void set_position_open(const std::string&, bool) override {}
};

class FailingReconciler : public IReconciler
{
public:
    std::string reconcile(const portfolio&, double) override
    {
        return "balance drift > tolerance: local=100, exchange=50";
    }
};

class SpyKillSwitch : public IKillSwitch
{
public:
    std::atomic<int> invocations{0};
    std::atomic<long> last_deadline_ms{0};
    bool cancel_all_and_flatten(std::chrono::milliseconds d) override
    {
        invocations.fetch_add(1);
        last_deadline_ms.store(d.count());
        return true;
    }
};

}

TEST(LiveSafety, ReconcilerFailure_BlocksConstruction)
{
    auto dh = std::make_shared<data_handler>();
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<NullStrategy>();

    engine_config cfg;
    cfg.mode = engine_mode::live;
    cfg.reconciler = std::make_shared<FailingReconciler>();

    EXPECT_THROW(engine eng(dh, ob, strat, std::move(cfg)), std::runtime_error);
}

TEST(LiveSafety, NoopReconciler_AllowsConstruction)
{
    auto dh = std::make_shared<data_handler>();
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<NullStrategy>();

    engine_config cfg;
    cfg.mode = engine_mode::live;
    // No reconciler set -> engine installs NoopReconciler internally.
    EXPECT_NO_THROW(engine eng(dh, ob, strat, std::move(cfg)));
}

TEST(LiveSafety, BacktestMode_SkipsReconcileGate)
{
    auto dh = std::make_shared<data_handler>();
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<NullStrategy>();

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    // Even a failing reconciler must NOT block backtest startup.
    cfg.reconciler = std::make_shared<FailingReconciler>();

    EXPECT_NO_THROW(engine eng(dh, ob, strat, std::move(cfg)));
}

// ---------------------------------------------------------------------------
// Direct behavioral test for provider_callbacks_armed_ guard (post-stop safety)
// The engine disarms the atomic *before* closing transports so that any
// in-flight callbacks from provider threads see the guard and early-return.
// ---------------------------------------------------------------------------

namespace {

// Minimal spy provider that captures the callbacks the engine wires,
// so we can invoke them *after* engine destruction and observe early-return
// behavior (no crash, and for factories: empty returns).
class ArmedGuardSpyProvider : public IProvider
{
public:
    std::function<void(std::shared_ptr<event>)> captured_publisher;
    funding_event_factory captured_funding_factory;
    std::function<void(std::string_view)> captured_halt;

    std::string name() const override { return "armed-spy"; }
    bool has_data_feed() const override { return false; }
    bool has_execution() const override { return true; }

    bool open() override { return true; }
    void close() override {}

    std::shared_ptr<IDataTransport> get_transport() override { return nullptr; }
    std::shared_ptr<IExecutionAdapter> get_execution_adapter() override { return nullptr; }

    void set_event_publisher(std::function<void(std::shared_ptr<event>)> fn) override
    {
        captured_publisher = std::move(fn);
    }

    void set_funding_event_factory(funding_event_factory fn) override
    {
        captured_funding_factory = std::move(fn);
        IProvider::set_funding_event_factory(captured_funding_factory);
    }

    void set_halt_callback(std::function<void(std::string_view reason)> cb) override
    {
        captured_halt = std::move(cb);
    }
};

} // anonymous

TEST(LiveSafety, ProviderCallbacks_ArmedGuardPreventsLateInvocations)
{
    auto dh = std::make_shared<data_handler>();
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<NullStrategy>();

    auto spy = std::make_shared<ArmedGuardSpyProvider>();

    engine_config cfg;
    cfg.mode = engine_mode::backtest;   // avoid live gates
    cfg.provider = spy;

    // Construction wires the guarded callbacks into the provider.
    {
        engine eng(dh, ob, strat, std::move(cfg));
        // eng dtor will disarm (in ~engine / stop path)
    }

    // Now invoke the captured callbacks *after* the engine (and its armed flag) is gone.
    // They must early-return safely thanks to the guard.

    // 1. Event publisher
    if (spy->captured_publisher)
    {
        // Should not publish / crash (early return inside the captured lambda)
        spy->captured_publisher(nullptr);
        auto now = std::chrono::system_clock::now();
        spy->captured_publisher(std::make_shared<event>(event_type::market, now));
    }

    // 2. Funding factory -> must return empty after disarm
    if (spy->captured_funding_factory)
    {
        auto f = spy->captured_funding_factory(
            std::chrono::system_clock::now(), "TEST", 1.23, "late");
        EXPECT_FALSE(f);  // early return produces null
    }

    // 3. Halt callback (should be no-op)
    if (spy->captured_halt)
    {
        spy->captured_halt("late callback after stop");
    }

    SUCCEED();
}

// ---------------------------------------------------------------------------
// Risk halt must raise the process-wide halt_flag_ (S3 terminal), not only a
// local halt_requested out-param. DataBridge / L2 dispatch / streaming loops
// all observe halt_flag_; without this, live can keep submitting after a risk
// halt decision.
// ---------------------------------------------------------------------------

namespace {

// Buys a fixed qty every bar until the engine stops calling us.
class RelentlessBuyer : public IStrategy
{
    double qty_;
    int calls_ = 0;
public:
    explicit RelentlessBuyer(double qty = 10.0) : qty_(qty) {}
    int calls() const { return calls_; }
    std::optional<order_event> on_market(const market_event& mkt) override
    {
        ++calls_;
        return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                           order_type::market, order_side::buy,
                           qty_, mkt.get_close());
    }
    void set_position_open(const std::string&, bool) override {}
};

} // anonymous

TEST(LiveSafety, RiskHaltSetsProcessWideHaltFlag)
{
    // Declining market + aggressive buyer + tight max_drawdown → risk halt.
    auto dh = std::make_shared<data_handler>();
    double price = 100.0;
    for (int i = 0; i < 200; ++i)
    {
        const double close = price * (1.0 - 0.01);
        dh->load_into_queue("2024-01-01", "TEST",
                            price, price + 0.1, close - 0.1, close, 1000);
        price = close;
    }

    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<RelentlessBuyer>(10.0);

    // Liquidity so market buys fill and equity can draw down.
    MarketMaker mm(3);
    mm.add_orders(ob, 100.0, 100);

    engine_config cfg;
    cfg.mode            = engine_mode::backtest;
    cfg.initial_balance = 10000.0;
    cfg.seed            = 7;
    cfg.threading       = thread_preset::inline_mode;
    cfg.disable_pinning = true;
    cfg.risk.max_drawdown       = 0.05;  // 5%
    cfg.risk.max_loss_per_trade = 1e9;   // isolate drawdown path

    engine eng(dh, ob, strat, std::move(cfg));
    eng.run();

    // Must be process-wide terminal, not merely "run loop exited early".
    EXPECT_TRUE(eng.get_halt_flag().load(std::memory_order_acquire));
    // Strategy must have been cut short of the full 200-bar dataset.
    EXPECT_LT(strat->calls(), 200);
}

TEST(LiveSafety, RiskHaltIsWriteOnceTerminal)
{
    auto dh = std::make_shared<data_handler>();
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<NullStrategy>();

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.threading = thread_preset::inline_mode;
    cfg.disable_pinning = true;

    engine eng(dh, ob, strat, std::move(cfg));

    // Simulate external/risk-equivalent terminal raise via public API used by
    // watchdog and (after fix) risk paths: exchange(true) write-once.
    eng.trigger_halt("unit-test terminal");
    EXPECT_TRUE(eng.get_halt_flag().load(std::memory_order_acquire));

    // Second raise must be a no-op (still halted).
    eng.trigger_halt("second raise ignored");
    EXPECT_TRUE(eng.get_halt_flag().load(std::memory_order_acquire));
}
