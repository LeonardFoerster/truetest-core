// Engine-level integration test for the venue risk-check wiring added
// in step twelve. Closes the gap flagged in that commit's report:
// the unit logic in test_futures_risk_check covers the cap math, but
// before this file there was nothing asserting that the engine
// actually consults provider->get_risk_check() in the order-routing
// path and honors a rejection.
// Strategy: stub a minimal MockProvider that exposes a controllable
// MockRiskCheck. Run the engine on synthetic bars. The TestStrategy
// emits a BUY at bar 3; we verify whether the engine rejects or
// allows it by inspecting fills.

#include <gtest/gtest.h>

#include "engine/engine.h"
#include "engine/engine_config.h"
#include "data/data_handler.h"
#include "orderbook/orderbook.h"
#include "providers/provider.h"
#include "risk/futures_risk_check.h"

#include <atomic>
#include <iostream>
#include <memory>
#include <sstream>

namespace {

// Same silence helper as the rest of the engine tests. Anonymous-namespaced
// to avoid colliding with siblings under LTO.
struct SilenceCout
{
    std::ostringstream sink;
    std::streambuf* orig;
    SilenceCout() : sink(), orig(std::cout.rdbuf(sink.rdbuf())) {}
    ~SilenceCout() { std::cout.rdbuf(orig); }
};

class CountingRiskCheck : public IRiskCheck
{
public:
    explicit CountingRiskCheck(bool reject) : reject_(reject) {}

    int call_count() const { return calls_.load(); }

    decision evaluate(const order_event&,
                      const portfolio&,
                      double) const override
    {
        ++calls_;
        if (reject_) return {false, "mock-rejected: cap simulated"};
        return {};
    }

private:
    bool reject_;
    mutable std::atomic<int> calls_{0};
};

// Minimal IProvider stub. Returns nullptr from every hook except
// get_risk_check(); the engine wires that one through. We deliberately
// report has_data_feed=false so engine.run() falls back to the
// data_handler-driven path instead of looking for a transport.
class MockProvider : public IProvider
{
public:
    explicit MockProvider(std::shared_ptr<IRiskCheck> rc)
        : rc_(std::move(rc))
    {}

    std::string name() const override { return "mock"; }
    bool has_data_feed() const override { return false; }
    bool has_execution() const override { return false; }
    bool open() override { return true; }
    void close() override {}

    std::shared_ptr<IDataTransport>    get_transport() override { return nullptr; }
    std::shared_ptr<IExecutionAdapter> get_execution_adapter() override { return nullptr; }
    std::shared_ptr<IRiskCheck>        get_risk_check() override { return rc_; }

private:
    std::shared_ptr<IRiskCheck> rc_;
};

// Strategy that emits a single BUY on bar 3 and a single SELL on bar 6.
// Mirrors the convention used elsewhere in test_engine.cpp; copying
// rather than including avoids cross-TU class-name collisions under LTO.
class BuyAtBarThreeStrategy : public IStrategy
{
public:
    std::optional<order_event> on_market(const market_event& mkt) override
    {
        ++calls_;
        if (calls_ == 3 && !position_open_)
            return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                               order_type::market, order_side::buy,
                               1.0, mkt.get_close());
        if (calls_ == 6 && position_open_)
            return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                               order_type::market, order_side::sell,
                               1.0, mkt.get_close());
        return std::nullopt;
    }

    void set_position_open(const std::string&, bool open) override
    {
        position_open_ = open;
    }

private:
    int calls_ = 0;
    bool position_open_ = false;
};

std::shared_ptr<data_handler> make_bars(int n)
{
    auto dh = std::make_shared<data_handler>();
    for (int i = 0; i < n; ++i)
        dh->load_into_queue("2024-01-01", "TEST",
                            100.0 + i, 105.0 + i, 95.0 + i, 102.0 + i, 1000);
    return dh;
}

}

// Without a venue risk check, the engine emits and fills both legs
// - establishes the baseline so the rejection test below has a clean
// "before" state to compare against.
TEST(EngineVenueRiskCheck, BaselineWithoutRiskCheckFills)
{
    SilenceCout quiet;

    auto dh = make_bars(10);
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<BuyAtBarThreeStrategy>();

    engine_config cfg;
    // No provider -> engine.risk_check_ stays null, normal path runs.

    engine eng(dh, ob, strat, cfg);
    eng.run();

    EXPECT_GT(eng.get_analytics().snapshot().total_fills, 0u)
        << "baseline run must produce fills - strategy or harness broke";
}

TEST(EngineVenueRiskCheck, RejectsOrderWhenCheckRefuses)
{
    SilenceCout quiet;

    auto dh = make_bars(10);
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<BuyAtBarThreeStrategy>();
    auto rc = std::make_shared<CountingRiskCheck>(/*reject=*/true);

    engine_config cfg;
    cfg.provider = std::make_shared<MockProvider>(rc);

    engine eng(dh, ob, strat, cfg);
    eng.run();

    EXPECT_GT(rc->call_count(), 0)
        << "engine must invoke provider->get_risk_check() per order";
    // BUY rejected -> no position -> SELL never emitted -> zero fills.
    // Cleanest end-to-end signal that the rejection path is wired.
    EXPECT_EQ(eng.get_analytics().snapshot().total_fills, 0u)
        << "rejected orders must not produce fills";
}

TEST(EngineVenueRiskCheck, AllowsOrderWhenCheckPasses)
{
    SilenceCout quiet;

    auto dh = make_bars(10);
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<BuyAtBarThreeStrategy>();
    auto rc = std::make_shared<CountingRiskCheck>(/*reject=*/false);

    engine_config cfg;
    cfg.provider = std::make_shared<MockProvider>(rc);

    engine eng(dh, ob, strat, cfg);
    eng.run();

    EXPECT_GT(rc->call_count(), 0)
        << "engine must invoke the check even when it allows";
    EXPECT_GT(eng.get_analytics().snapshot().total_fills, 0u)
        << "allow=true must not block the existing fill path";
}

// Captures engine event publisher so tests can inject funding while the
// engine is still armed (mirrors live user-data funding path).
class PublisherSpyProvider : public IProvider
{
public:
    using publisher_fn = std::function<void(std::shared_ptr<event>)>;

    std::string name() const override { return "pub-spy"; }
    bool has_data_feed() const override { return false; }
    bool has_execution() const override { return false; }
    bool open() override { return true; }
    void close() override {}
    std::shared_ptr<IDataTransport> get_transport() override { return nullptr; }
    std::shared_ptr<IExecutionAdapter> get_execution_adapter() override { return nullptr; }

    void set_event_publisher(publisher_fn fn) override
    {
        pub_ = std::move(fn);
        IProvider::set_event_publisher(pub_);
    }

    publisher_fn pub_;
};

TEST(EngineVenueRiskCheck, FundingEventUpdatesAnalyticsRiskView)
{
    SilenceCout quiet;

    auto dh = make_bars(3);
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<BuyAtBarThreeStrategy>();
    auto spy = std::make_shared<PublisherSpyProvider>();

    engine_config cfg;
    cfg.provider = spy;
    cfg.initial_balance = 100000.0;

    engine eng(dh, ob, strat, cfg);

    // Publisher is wired in the constructor when provider is set.
    ASSERT_TRUE(static_cast<bool>(spy->pub_))
        << "engine must wire provider event publisher at construct";

    auto funding = std::make_shared<funding_event>(
        std::chrono::system_clock::now(), "TEST", 0.0, 250.0, "FUNDING_FEE");
    spy->pub_(funding);

    EXPECT_NEAR(eng.get_analytics().risk_view().equity, 100250.0, 1e-6)
        << "funding must update engine analytics (risk_view), not only portfolio";
    EXPECT_NEAR(eng.get_analytics().total_funding_pnl(), 250.0, 1e-6);

    eng.run();  // clean teardown path
}

// Records the mark_price passed into evaluate so we can assert symbol-bound
// mids (not a stale other-symbol last mid).
class MarkCaptureRiskCheck : public IRiskCheck
{
public:
    mutable std::vector<double> marks;

    decision evaluate(const order_event&,
                      const portfolio&,
                      double mark_price) const override
    {
        marks.push_back(mark_price);
        return {};
    }
};

TEST(EngineVenueRiskCheck, VenueRiskCheckReceivesSymbolMarkFromBars)
{
    SilenceCout quiet;

    auto dh = make_bars(10);
    auto ob = std::make_shared<orderbook>();
    auto strat = std::make_shared<BuyAtBarThreeStrategy>();
    auto rc = std::make_shared<MarkCaptureRiskCheck>();

    engine_config cfg;
    cfg.provider = std::make_shared<MockProvider>(rc);

    engine eng(dh, ob, strat, cfg);
    eng.run();

    ASSERT_FALSE(rc->marks.empty()) << "risk check must be consulted";
    // Order may be latency-deferred to the next bar open, so the exact
    // mid is path-dependent — but it must be this symbol's tracked mid
    // (make_bars opens/closes live in [100, 111]), never 0 / unknown.
    for (double m : rc->marks) {
        EXPECT_GT(m, 0.0) << "symbol-bound mark must not be missing";
        EXPECT_GE(m, 100.0);
        EXPECT_LE(m, 111.0);
    }
}
