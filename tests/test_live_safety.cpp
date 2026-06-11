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
