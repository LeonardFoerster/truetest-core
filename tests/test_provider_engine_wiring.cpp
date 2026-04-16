#include <gtest/gtest.h>

#include "core/engine.h"
#include "core/engine_config.h"
#include "data/data_handler.h"
#include "execution/execution_adapter.h"
#include "providers/provider.h"
#include "strategy/strategy_interface.h"

#include <memory>
#include <sstream>
#include <vector>

// Verifies the minimum contract of the refactored core: when an engine_config
// carries a provider with execution, the engine routes orders through the
// provider's execution adapter — with no HAS_* define needed. The test uses
// only the always-on build surface (no ENABLE_BINANCE, no ENABLE_WEB_UI, etc.).

namespace {

struct SilenceCout
{
    std::streambuf* orig;
    std::ostringstream sink;
    SilenceCout() : orig(std::cout.rdbuf(sink.rdbuf())) {}
    ~SilenceCout() { std::cout.rdbuf(orig); }
};

// Echoes every submitted order back as an immediate fill on the next poll.
class EchoExecutionAdapter : public IExecutionAdapter
{
public:
    int submit_count = 0;
    std::vector<fill_event> pending;

    void submit_order(const order_event& o) override
    {
        ++submit_count;
        double price = o.get_price() > 0.0 ? o.get_price() : 100.0;
        pending.emplace_back(o.get_timestamp(), o.get_symbol(),
                             o.get_order_id(), o.get_side(),
                             o.get_quantity(), price);
    }

    bool poll_fills(std::vector<fill_event>& out) override
    {
        if (pending.empty()) return false;
        out.insert(out.end(), pending.begin(), pending.end());
        pending.clear();
        return true;
    }
};

class FakeProvider : public IProvider
{
public:
    std::shared_ptr<EchoExecutionAdapter> adapter
        = std::make_shared<EchoExecutionAdapter>();

    std::string name() const override { return "fake"; }
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

// Fires exactly one market buy on the first bar, then stays idle.
class OneShotBuyStrategy : public IStrategy
{
    bool fired_ = false;
public:
    std::optional<order_event> on_market(const market_event& mkt) override
    {
        if (fired_) return std::nullopt;
        fired_ = true;
        return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                           order_type::market, order_side::buy,
                           1.0, mkt.get_close());
    }
    void set_position_open(const std::string&, bool) override {}
};

std::shared_ptr<data_handler> make_three_bars()
{
    auto dh = std::make_shared<data_handler>();
    dh->load_into_queue("2024-01-01", "TEST", 100.0, 101.0, 99.0, 100.0, 1000);
    dh->load_into_queue("2024-01-01", "TEST", 100.0, 102.0, 99.5, 101.0, 1000);
    dh->load_into_queue("2024-01-01", "TEST", 101.0, 103.0, 100.5, 102.0, 1000);
    return dh;
}

} // namespace

TEST(ProviderEngineWiring, EngineRoutesOrdersThroughProviderAdapter)
{
    SilenceCout quiet;

    auto dh = make_three_bars();
    auto fake = std::make_shared<FakeProvider>();
    auto strat = std::make_shared<OneShotBuyStrategy>();

    engine_config cfg;
    cfg.provider = fake;

    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.run();

    EXPECT_EQ(fake->adapter->submit_count, 1);
}

TEST(ProviderEngineWiring, LifecycleStateDefaultsClosed)
{
    FakeProvider p;
    EXPECT_EQ(p.lifecycle_state(), IProvider::lifecycle::closed);
}

TEST(ProviderEngineWiring, ConfigureAndOnMidPriceAreNoopByDefault)
{
    FakeProvider p;
    engine_config cfg;
    EXPECT_NO_THROW(p.configure(cfg));
    EXPECT_NO_THROW(p.on_mid_price("TEST", 100.0));
}
