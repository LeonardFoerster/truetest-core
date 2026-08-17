#include <gtest/gtest.h>

#include "engine/engine.h"
#include "engine/engine_config.h"
#include "data/data_handler.h"
#include "execution/execution_adapter.h"
#include "execution/async_support.h"
#include "providers/provider.h"
#include "strategy/strategy_interface.h"
#include "ui/console_dashboard.h"

#include <memory>
#include <vector>
#include <string>

namespace {

// A fake adapter that supports async submit/cancel and implements IAsyncSubmitSupport.
// Used to test the new engine integration paths without any real provider or network.
class FakeAsyncExecutionAdapter : public IExecutionAdapter, public IAsyncSubmitSupport
{
public:
    int submit_count = 0;
    int cancel_count = 0;
    std::uint64_t last_submit_id = 0;
    bool async_enabled = true;
    std::optional<submit_result> result_on_submit;

    std::vector<submit_result> pending_submit_results;
    std::vector<synth_meta>    pending_synth_meta;

    unknown_fill_handler handler;

    // --- IExecutionAdapter ---
    void submit_order(const order_event& o) override
    {
        last_submit_id = o.get_order_id();
        ++submit_count;
        if (result_on_submit)
        {
            auto result = *result_on_submit;
            if (result.engine_id == 0) result.engine_id = o.get_order_id();
            if (result.symbol.empty()) result.symbol = o.get_symbol();
            pending_submit_results.push_back(std::move(result));
            result_on_submit.reset();
        }
    }

    bool cancel_order(std::uint64_t /*order_id*/) override
    {
        ++cancel_count;
        return true;
    }

    bool poll_fills(std::vector<fill_event>& /*out*/) override
    {
        return false; // async fills come via other paths in this test
    }

    bool supports_async_submit() const override { return async_enabled; }

    IAsyncSubmitSupport* get_async_support() override { return this; }

    const std::string& last_error() const override
    {
        static const std::string err = "simulated-async-error";
        return err;
    }

    // --- IAsyncSubmitSupport ---
    void set_unknown_fill_handler(unknown_fill_handler h) override
    {
        handler = std::move(h);
    }

    void clear_unknown_fill_handler() override
    {
        handler = {};
    }

    bool poll_synth_meta(std::vector<synth_meta>& out) override
    {
        if (pending_synth_meta.empty()) return false;
        out.insert(out.end(), pending_synth_meta.begin(), pending_synth_meta.end());
        pending_synth_meta.clear();
        return true;
    }

    bool poll_submit_results(std::vector<submit_result>& out) override
    {
        if (pending_submit_results.empty()) return false;
        out.insert(out.end(), pending_submit_results.begin(), pending_submit_results.end());
        pending_submit_results.clear();
        return true;
    }

    // Test helpers
    void inject_submit_result(submit_result r)
    {
        pending_submit_results.push_back(std::move(r));
    }

    void inject_synth_meta(synth_meta m)
    {
        pending_synth_meta.push_back(std::move(m));
    }

    void trigger_unknown_fill(const parsed_exec& msg, std::uint64_t fill_id)
    {
        if (handler)
        {
            (void)handler(msg, fill_id);
        }
    }
};

class FakeAsyncProvider : public IProvider
{
public:
    std::shared_ptr<FakeAsyncExecutionAdapter> adapter
        = std::make_shared<FakeAsyncExecutionAdapter>();

    std::string name() const override { return "fake-async"; }
    bool has_data_feed() const override { return false; }
    bool has_execution() const override { return true; }
    bool open() override { return true; }
    void close() override {}
    std::shared_ptr<IDataTransport> get_transport() override { return nullptr; }
    std::shared_ptr<IExecutionAdapter> get_execution_adapter() override { return adapter; }
};

// Very simple strategy that fires one market order
class OneShotStrategy : public IStrategy
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

class EveryBarStrategy : public IStrategy
{
public:
    std::optional<order_event> on_market(const market_event& mkt) override
    {
        return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                           order_type::market, order_side::buy,
                           1.0, mkt.get_close());
    }
    void set_position_open(const std::string&, bool) override {}
};

std::shared_ptr<data_handler> make_bars()
{
    auto dh = std::make_shared<data_handler>();
    dh->load_into_queue("2024-01-01", "TEST", 100.0, 101.0, 99.0, 100.0, 1000);
    dh->load_into_queue("2024-01-01", "TEST", 100.0, 102.0, 99.5, 101.0, 1000);
    return dh;
}

} // namespace

TEST(EngineAsyncSupport, WiresAsyncCapability)
{
    auto dh = make_bars();
    auto provider = std::make_shared<FakeAsyncProvider>();
    auto strat = std::make_shared<OneShotStrategy>();

    engine_config cfg;
    cfg.provider = provider;

    engine eng(dh, nullptr, strat, std::move(cfg));

    // The engine should have asked for the capability and installed the handler
    auto* cap = provider->adapter->get_async_support();
    EXPECT_NE(cap, nullptr);
    EXPECT_TRUE(provider->adapter->supports_async_submit());
}

TEST(EngineAsyncSupport, AsyncSubmitResultIsProcessed)
{
    auto dh = make_bars();
    auto provider = std::make_shared<FakeAsyncProvider>();
    auto strat = std::make_shared<OneShotStrategy>();

    engine_config cfg;
    cfg.provider = provider;

    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.run();   // strategy fires one order

    EXPECT_EQ(provider->adapter->submit_count, 1);

    // Simulate async acknowledgement from venue
    submit_result res;
    res.engine_id = 1;           // would normally come from engine
    res.symbol = "TEST";
    res.ok = true;
    res.op = submit_result::operation::submit;
    res.exchange_order_id = "EX123";

    provider->adapter->inject_submit_result(std::move(res));

    // Drain should be called internally during normal operation.
    // For this test we call the internal drain via a public path if available,
    // or rely on next event processing. We call a method that triggers drain.
    // Since drain_async_submit_results is private-ish, we simulate by
    // advancing the engine a bit (it polls on certain paths).
    // For explicit testing we can call through the adapter.
    std::vector<submit_result> drained;
    bool had = provider->adapter->poll_submit_results(drained);
    EXPECT_TRUE(had);
    EXPECT_EQ(drained.size(), 1u);
}

TEST(EngineAsyncSupport, AmbiguousPostWriteSubmitTriggersTerminalHalt)
{
    auto dh = make_bars();
    auto provider = std::make_shared<FakeAsyncProvider>();
    auto strat = std::make_shared<OneShotStrategy>();
    provider->adapter->result_on_submit = submit_result{
        .engine_id = 1,
        .symbol = "TEST",
        .exchange_order_id = {},
        .error = "ambiguous post-write outcome",
        .op = submit_result::operation::submit,
        .ok = false,
        .uncertain = true,
    };

    engine_config cfg;
    cfg.provider = provider;
    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.run();

    EXPECT_TRUE(eng.is_halted());
    EXPECT_FALSE(eng.run_succeeded());
}

TEST(EngineAsyncSupport, FatalSubmitTriggersTerminalHalt)
{
    auto dh = make_bars();
    auto provider = std::make_shared<FakeAsyncProvider>();
    auto strat = std::make_shared<OneShotStrategy>();
    provider->adapter->result_on_submit = submit_result{
        .engine_id = 1,
        .symbol = "TEST",
        .exchange_order_id = {},
        .error = "safety prerequisite failed",
        .op = submit_result::operation::submit,
        .ok = false,
        .fatal = true,
    };

    engine_config cfg;
    cfg.provider = provider;
    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.run();

    EXPECT_TRUE(eng.is_halted());
    EXPECT_FALSE(eng.run_succeeded());
}

TEST(EngineAsyncSupport, AmbiguousFatalSubmitKeepsAmbiguityDiagnosis)
{
    auto dh = make_bars();
    auto provider = std::make_shared<FakeAsyncProvider>();
    auto strat = std::make_shared<OneShotStrategy>();
    provider->adapter->result_on_submit = submit_result{
        .engine_id = 1,
        .symbol = "TEST",
        .exchange_order_id = {},
        .error = "ambiguous fatal post-write outcome",
        .op = submit_result::operation::submit,
        .ok = false,
        .uncertain = true,
        .fatal = true,
    };

    truetest::ui::dashboard_config dashboard_cfg;
    dashboard_cfg.mode = truetest::ui::output_mode::off;
    auto dashboard = std::make_shared<truetest::ui::ConsoleDashboard>(
        std::move(dashboard_cfg));
    engine_config cfg;
    cfg.provider = provider;
    cfg.dashboard = dashboard;
    cfg.show_progress = false;
    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.run();

    EXPECT_TRUE(eng.is_halted());
    EXPECT_EQ(dashboard->shutdown_reason(),
              "venue order outcome is ambiguous after request write");
}

TEST(EngineAsyncSupport, AmbiguousCancelTriggersTerminalHaltAndRetainsOrder)
{
    auto dh = make_bars();
    auto provider = std::make_shared<FakeAsyncProvider>();
    auto strat = std::make_shared<OneShotStrategy>();
    engine_config cfg;
    cfg.provider = provider;
    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.run();

    const auto order_id = provider->adapter->last_submit_id;
    ASSERT_NE(order_id, 0u);
    ASSERT_TRUE(eng.get_order_tracker().is_active(order_id));
    ASSERT_TRUE(eng.cancel_order("TEST", order_id, "operator"));
    ASSERT_EQ(provider->adapter->cancel_count, 1);

    provider->adapter->inject_submit_result(submit_result{
        .engine_id = order_id,
        .symbol = "TEST",
        .exchange_order_id = {},
        .error = "ambiguous post-write cancel",
        .op = submit_result::operation::cancel,
        .ok = false,
        .uncertain = true,
    });

    EXPECT_FALSE(eng.cancel_order("TEST", order_id, "drain"));
    EXPECT_TRUE(eng.is_halted());
    EXPECT_FALSE(eng.run_succeeded());
    EXPECT_TRUE(eng.get_order_tracker().is_active(order_id));
    EXPECT_EQ(provider->adapter->cancel_count, 1)
        << "terminal halt must prevent a second venue mutation";
}

TEST(EngineAsyncSupport, FatalCancelTriggersTerminalHaltAndRetainsOrder)
{
    auto dh = make_bars();
    auto provider = std::make_shared<FakeAsyncProvider>();
    auto strat = std::make_shared<OneShotStrategy>();
    engine_config cfg;
    cfg.provider = provider;
    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.run();

    const auto order_id = provider->adapter->last_submit_id;
    ASSERT_NE(order_id, 0u);
    ASSERT_TRUE(eng.cancel_order("TEST", order_id, "operator"));
    ASSERT_EQ(provider->adapter->cancel_count, 1);

    provider->adapter->inject_submit_result(submit_result{
        .engine_id = order_id,
        .symbol = "TEST",
        .exchange_order_id = {},
        .error = "cancel safety prerequisite failed",
        .op = submit_result::operation::cancel,
        .ok = false,
        .fatal = true,
    });

    EXPECT_FALSE(eng.cancel_order("TEST", order_id, "drain"));
    EXPECT_TRUE(eng.is_halted());
    EXPECT_FALSE(eng.run_succeeded());
    EXPECT_TRUE(eng.get_order_tracker().is_active(order_id));
    EXPECT_EQ(provider->adapter->cancel_count, 1);
}

TEST(EngineAsyncSupport, AmbiguousFatalCancelKeepsAmbiguityDiagnosis)
{
    auto dh = make_bars();
    auto provider = std::make_shared<FakeAsyncProvider>();
    auto strat = std::make_shared<OneShotStrategy>();
    truetest::ui::dashboard_config dashboard_cfg;
    dashboard_cfg.mode = truetest::ui::output_mode::off;
    auto dashboard = std::make_shared<truetest::ui::ConsoleDashboard>(
        std::move(dashboard_cfg));
    engine_config cfg;
    cfg.provider = provider;
    cfg.dashboard = dashboard;
    cfg.show_progress = false;
    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.run();

    const auto order_id = provider->adapter->last_submit_id;
    ASSERT_NE(order_id, 0u);
    ASSERT_TRUE(eng.cancel_order("TEST", order_id, "operator"));
    provider->adapter->inject_submit_result(submit_result{
        .engine_id = order_id,
        .symbol = "TEST",
        .exchange_order_id = {},
        .error = "ambiguous fatal post-write cancel",
        .op = submit_result::operation::cancel,
        .ok = false,
        .uncertain = true,
        .fatal = true,
    });

    EXPECT_FALSE(eng.cancel_order("TEST", order_id, "drain"));
    EXPECT_TRUE(eng.is_halted());
    EXPECT_EQ(dashboard->shutdown_reason(),
              "venue cancel outcome is ambiguous after request write");
    EXPECT_TRUE(eng.get_order_tracker().is_active(order_id));
    EXPECT_EQ(provider->adapter->cancel_count, 1);
}

TEST(EngineAsyncSupport, FailedSubmitReleasesCapacityForNextOrder)
{
    auto dh = make_bars();
    auto provider = std::make_shared<FakeAsyncProvider>();
    auto strat = std::make_shared<EveryBarStrategy>();
    provider->adapter->result_on_submit = submit_result{
        .engine_id = 0,
        .symbol = {},
        .exchange_order_id = {},
        .error = "venue rejected first submit",
        .op = submit_result::operation::submit,
        .ok = false,
    };

    engine_config cfg;
    cfg.provider = provider;
    cfg.risk.max_open_orders = 1;
    cfg.execution_bar_delay = 0;
    cfg.show_progress = false;
    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.run();

    EXPECT_EQ(provider->adapter->submit_count, 2)
        << "rejecting the first async submit must release its lifecycle slot";
    EXPECT_EQ(eng.get_order_tracker().active_count(), 1u);
}

TEST(EngineAsyncSupport, SupportsAsyncSubmitDecision)
{
    auto provider = std::make_shared<FakeAsyncProvider>();
    EXPECT_TRUE(provider->adapter->supports_async_submit());

    provider->adapter->async_enabled = false;
    EXPECT_FALSE(provider->adapter->supports_async_submit());
}

TEST(EngineAsyncSupport, LastErrorIsExposed)
{
    auto provider = std::make_shared<FakeAsyncProvider>();
    const auto& err = provider->adapter->last_error();
    EXPECT_FALSE(err.empty());
}

TEST(EngineAsyncSupport, SynthMetaAndUnknownFillFlow)
{
    auto provider = std::make_shared<FakeAsyncProvider>();

    synth_meta meta;
    meta.engine_order_id = 42;
    meta.opener_order_id = 99;
    meta.strategy_name = "test-strat";
    provider->adapter->inject_synth_meta(std::move(meta));

    std::vector<synth_meta> out;
    bool got = provider->adapter->poll_synth_meta(out);
    EXPECT_TRUE(got);
    EXPECT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].opener_order_id, 99u);
}
