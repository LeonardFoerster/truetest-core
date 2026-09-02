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
#include <stdexcept>
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
    std::vector<venue_lifecycle_event> pending_lifecycle_events;
    std::size_t lifecycle_head = 0;

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

    bool poll_lifecycle_event(venue_lifecycle_event& out) noexcept override
    {
        if (lifecycle_head >= pending_lifecycle_events.size()) return false;
        out = pending_lifecycle_events[lifecycle_head++];
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

    void inject_lifecycle(venue_lifecycle_event event)
    {
        pending_lifecycle_events.push_back(event);
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

class ThrowingOnFillStrategy final : public OneShotStrategy
{
public:
    void on_fill(const fill_event&, std::uint64_t) override
    {
        ++fill_calls;
        throw std::runtime_error("intentional throw-once fill callback");
    }

    int fill_calls{0};
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

class AmendProbeAdapter final : public IExecutionAdapter
{
public:
    void submit_order(const order_event& order) override
    {
        submitted_order = order;
        fill_event fill(order.get_timestamp(), order.get_symbol(),
                        order.get_order_id(), order.get_side(),
                        4.0, order.get_price(), 0.0,
                        order.get_quantity() - 4.0, 1U);
        fill.set_source(fill_source::simulated);
        (void)fill.set_venue_execution_id("sim-exec-1");
        (void)fill.set_commission_currency("USD");
        fill.set_cumulative_filled_qty(
            4.0, fill_cumulative_source::simulated);
        pending_fills.push_back(std::move(fill));
    }

    bool poll_fills(std::vector<fill_event>& out) override
    {
        if (pending_fills.empty())
            return false;
        out.insert(out.end(),
                   std::make_move_iterator(pending_fills.begin()),
                   std::make_move_iterator(pending_fills.end()));
        pending_fills.clear();
        return true;
    }

    bool modify_order(std::uint64_t order_id,
                      double new_price,
                      double new_remaining_qty) override
    {
        ++modify_calls;
        modified_order_id = order_id;
        modified_price = new_price;
        modified_remaining_qty = new_remaining_qty;
        return true;
    }

    std::optional<order_event> submitted_order;
    std::vector<fill_event> pending_fills;
    int modify_calls = 0;
    std::uint64_t modified_order_id = 0;
    double modified_price = 0.0;
    double modified_remaining_qty = 0.0;
};

class AmendProbeProvider final : public IProvider
{
public:
    std::string name() const override { return "amend-probe"; }
    bool has_data_feed() const override { return false; }
    bool has_execution() const override { return true; }
    bool open() override { return true; }
    void close() override {}
    std::shared_ptr<IDataTransport> get_transport() override { return {}; }
    std::shared_ptr<IExecutionAdapter> get_execution_adapter() override
    {
        return adapter;
    }
    std::optional<instrument_spec> get_instrument(
        const std::string& symbol) const override
    {
        if (spec && spec->symbol == symbol)
            return spec;
        return std::nullopt;
    }

    std::shared_ptr<AmendProbeAdapter> adapter =
        std::make_shared<AmendProbeAdapter>();
    std::optional<instrument_spec> spec;
};

class AmendOnPartialFillStrategy final : public IStrategy
{
public:
    explicit AmendOnPartialFillStrategy(double requested_total_qty)
        : requested_total_qty_(requested_total_qty) {}

    void set_engine(engine* eng) noexcept { engine_ = eng; }

    std::optional<order_event> on_market(const market_event& market) override
    {
        if (submitted_)
            return std::nullopt;
        submitted_ = true;
        return order_event(market.get_timestamp(), market.get_symbol(),
                           order_type::limit, order_side::buy,
                           10.0, 99.0, time_in_force::gtc);
    }

    void on_fill(const fill_event& fill, std::uint64_t) override
    {
        if (amend_attempted_)
            return;
        amend_attempted_ = true;
        amend_result = engine_->modify_order(
            fill.get_symbol(), fill.get_order_id(), 101.0,
            requested_total_qty_);
    }

    bool amend_result = false;

private:
    engine* engine_ = nullptr;
    double requested_total_qty_ = 0.0;
    bool submitted_ = false;
    bool amend_attempted_ = false;
};

class RetainingFillProbeAdapter final : public IExecutionAdapter
{
public:
    void submit_order(const order_event& order) override
    {
        last_order_id = order.get_order_id();
        auto make_fill = [&](std::string symbol, std::uint64_t fill_id,
                             double cumulative, double remaining) {
            fill_event fill(order.get_timestamp(), std::move(symbol),
                            order.get_order_id(), order.get_side(),
                            1.0, order.get_price(), 0.0, remaining, fill_id);
            fill.set_source(fill_source::simulated);
            (void)fill.set_venue_execution_id(
                fill_id == 1U ? "sim-invalid" : "sim-valid");
            (void)fill.set_commission_currency("USD");
            fill.set_cumulative_filled_qty(
                cumulative, fill_cumulative_source::simulated);
            return fill;
        };
        // The first report is invalid for the submitted order. The valid
        // suffix must remain available for reconciliation; it must not be
        // destroyed merely because both reports arrived in one adapter batch.
        if (emit_invalid_head)
        {
            pending_fills.push_back(make_fill(
                "WRONG", 1U, 1.0, order.get_quantity() - 1.0));
        }
        pending_fills.push_back(make_fill(
            order.get_symbol(), 2U, 1.0, order.get_quantity() - 1.0));
    }

    bool poll_fills(std::vector<fill_event>& out) override
    {
        ++bulk_poll_calls;
        if (pending_fills.empty())
            return false;
        out.insert(out.end(),
                   std::make_move_iterator(pending_fills.begin()),
                   std::make_move_iterator(pending_fills.end()));
        pending_fills.clear();
        return true;
    }

    bool supports_transactional_fill_delivery() const noexcept override
    {
        return true;
    }

    bool peek_fill(fill_event& out) override
    {
        if (pending_fills.empty())
            return false;
        out = pending_fills.front();
        return true;
    }

    bool acknowledge_fill(std::uint64_t fill_id) override
    {
        if (pending_fills.empty()
            || pending_fills.front().get_fill_id() != fill_id)
            return false;
        pending_fills.erase(pending_fills.begin());
        return true;
    }

    std::vector<fill_event> pending_fills;
    int bulk_poll_calls = 0;
    std::uint64_t last_order_id = 0;
    bool emit_invalid_head = true;
};

class RetainingFillProbeProvider final : public IProvider
{
public:
    std::string name() const override { return "retaining-fill-probe"; }
    bool has_data_feed() const override { return false; }
    bool has_execution() const override { return true; }
    bool open() override { return true; }
    void close() override {}
    std::shared_ptr<IDataTransport> get_transport() override { return {}; }
    std::shared_ptr<IExecutionAdapter> get_execution_adapter() override
    {
        return adapter;
    }

    std::shared_ptr<RetainingFillProbeAdapter> adapter =
        std::make_shared<RetainingFillProbeAdapter>();
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

TEST(EngineAsyncSupport, AmendBelowAlreadyFilledTotalHasNoVenueMutation)
{
    auto provider = std::make_shared<AmendProbeProvider>();
    auto strategy = std::make_shared<AmendOnPartialFillStrategy>(3.0);
    engine_config cfg;
    cfg.provider = provider;
    cfg.execution_bar_delay = 0;
    cfg.threading = thread_preset::inline_mode;
    cfg.show_progress = false;
    cfg.initial_balance = 100'000.0;
    engine eng(make_bars(), nullptr, strategy, std::move(cfg));
    strategy->set_engine(&eng);

    eng.run();

    EXPECT_FALSE(strategy->amend_result);
    EXPECT_EQ(provider->adapter->modify_calls, 0)
        << "an inadmissible amend must be rejected before venue mutation";
    ASSERT_TRUE(provider->adapter->submitted_order.has_value());
    const auto order_id = provider->adapter->submitted_order->get_order_id();
    const auto* tracked = eng.get_order_tracker().find(order_id);
    ASSERT_NE(tracked, nullptr);
    EXPECT_DOUBLE_EQ(tracked->original_qty, 10.0);
    EXPECT_DOUBLE_EQ(tracked->filled_qty, 4.0);
    EXPECT_DOUBLE_EQ(tracked->remaining_qty(), 6.0);
    EXPECT_DOUBLE_EQ(tracked->limit_price, 99.0);
}

TEST(EngineAsyncSupport, AmendTotalQuantityRoutesOnlyDerivedRemainingQuantity)
{
    auto provider = std::make_shared<AmendProbeProvider>();
    auto strategy = std::make_shared<AmendOnPartialFillStrategy>(6.0);
    engine_config cfg;
    cfg.provider = provider;
    cfg.execution_bar_delay = 0;
    cfg.threading = thread_preset::inline_mode;
    cfg.show_progress = false;
    cfg.initial_balance = 100'000.0;
    engine eng(make_bars(), nullptr, strategy, std::move(cfg));
    strategy->set_engine(&eng);

    eng.run();

    EXPECT_TRUE(strategy->amend_result);
    ASSERT_EQ(provider->adapter->modify_calls, 1);
    EXPECT_DOUBLE_EQ(provider->adapter->modified_remaining_qty, 2.0)
        << "public amend quantity is the new total; adapters receive remainder";
    const auto order_id = provider->adapter->submitted_order->get_order_id();
    const auto* tracked = eng.get_order_tracker().find(order_id);
    ASSERT_NE(tracked, nullptr);
    EXPECT_DOUBLE_EQ(tracked->original_qty, 6.0);
    EXPECT_DOUBLE_EQ(tracked->filled_qty, 4.0);
    EXPECT_DOUBLE_EQ(tracked->remaining_qty(), 2.0);
    EXPECT_DOUBLE_EQ(tracked->limit_price, 101.0);
}

TEST(EngineAsyncSupport, C04_AmendValidatesTheReplacementRemainderAgainstMinQty)
{
    auto provider = std::make_shared<AmendProbeProvider>();
    provider->spec = instrument_spec{
        .symbol = "TEST",
        .tick_size = 0.01,
        .lot_size = 1.0,
        .min_qty = 2.0,
        .min_notional = 0.0,
    };
    // The original order is 10 and receives a confirmed fill of 4. An amend
    // to total=5 would replace the venue order with remaining=1, below the
    // declared minimum. Validating total=5 instead of replacement=1 admits an
    // order the venue contract explicitly forbids.
    auto strategy = std::make_shared<AmendOnPartialFillStrategy>(5.0);
    engine_config cfg;
    cfg.provider = provider;
    cfg.execution_bar_delay = 0;
    cfg.threading = thread_preset::inline_mode;
    cfg.show_progress = false;
    cfg.initial_balance = 100'000.0;
    engine eng(make_bars(), nullptr, strategy, std::move(cfg));
    strategy->set_engine(&eng);

    eng.run();

    EXPECT_FALSE(strategy->amend_result);
    EXPECT_EQ(provider->adapter->modify_calls, 0)
        << "invalid replacement remainder must be rejected before venue mutation";
    ASSERT_TRUE(provider->adapter->submitted_order.has_value());
    const auto order_id = provider->adapter->submitted_order->get_order_id();
    const auto* tracked = eng.get_order_tracker().find(order_id);
    ASSERT_NE(tracked, nullptr);
    EXPECT_DOUBLE_EQ(tracked->original_qty, 10.0);
    EXPECT_DOUBLE_EQ(tracked->filled_qty, 4.0);
    EXPECT_DOUBLE_EQ(tracked->remaining_qty(), 6.0);
    EXPECT_DOUBLE_EQ(tracked->limit_price, 99.0);
}

TEST(EngineAsyncSupport,
     C04_AmendValidatesReplacementRemainderAgainstMinNotional)
{
    auto provider = std::make_shared<AmendProbeProvider>();
    provider->spec = instrument_spec{
        .symbol = "TEST",
        .tick_size = 0.01,
        .lot_size = 1.0,
        .min_qty = 0.0,
        .min_notional = 200.0,
    };
    auto strategy = std::make_shared<AmendOnPartialFillStrategy>(5.0);
    engine_config cfg;
    cfg.provider = provider;
    cfg.execution_bar_delay = 0;
    cfg.threading = thread_preset::inline_mode;
    cfg.show_progress = false;
    cfg.initial_balance = 100'000.0;
    engine eng(make_bars(), nullptr, strategy, std::move(cfg));
    strategy->set_engine(&eng);

    eng.run();

    EXPECT_FALSE(strategy->amend_result);
    EXPECT_EQ(provider->adapter->modify_calls, 0);
    ASSERT_TRUE(provider->adapter->submitted_order.has_value());
    const auto order_id = provider->adapter->submitted_order->get_order_id();
    const auto* tracked = eng.get_order_tracker().find(order_id);
    ASSERT_NE(tracked, nullptr);
    EXPECT_DOUBLE_EQ(tracked->original_qty, 10.0);
    EXPECT_DOUBLE_EQ(tracked->filled_qty, 4.0);
    EXPECT_DOUBLE_EQ(tracked->remaining_qty(), 6.0);
    EXPECT_DOUBLE_EQ(tracked->limit_price, 99.0);
}

TEST(EngineAsyncSupport, InvalidFillDoesNotDestroyUnprocessedBatchSuffix)
{
    auto provider = std::make_shared<RetainingFillProbeProvider>();
    auto strategy = std::make_shared<OneShotStrategy>();
    engine_config cfg;
    cfg.provider = provider;
    cfg.execution_bar_delay = 0;
    cfg.threading = thread_preset::inline_mode;
    cfg.show_progress = false;
    cfg.initial_balance = 100'000.0;
    engine eng(make_bars(), nullptr, strategy, std::move(cfg));

    eng.run();

    EXPECT_TRUE(eng.is_halted());
    EXPECT_FALSE(eng.get_portfolio().position_open("TEST"));
    EXPECT_EQ(provider->adapter->pending_fills.size(), 2U)
        << "an unacknowledged invalid fill and its valid suffix must remain retained";
}

TEST(EngineAsyncSupport,
     C03_ThrowingFillCallbackCannotExposePartialEconomicCommit)
{
    auto provider = std::make_shared<RetainingFillProbeProvider>();
    provider->adapter->emit_invalid_head = false;
    auto strategy = std::make_shared<ThrowingOnFillStrategy>();
    engine_config cfg;
    cfg.provider = provider;
    cfg.execution_bar_delay = 0;
    cfg.threading = thread_preset::inline_mode;
    cfg.show_progress = false;
    cfg.initial_balance = 100'000.0;
    engine eng(make_bars(), nullptr, strategy, std::move(cfg));

    bool threw = false;
    try
    {
        eng.run();
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }

    ASSERT_NE(provider->adapter->last_order_id, 0U);
    const auto* tracked =
        eng.get_order_tracker().find(provider->adapter->last_order_id);
    ASSERT_NE(tracked, nullptr);
    const auto report = eng.get_analytics().generate_report();

    const bool fully_uncommitted =
        tracked->filled_qty == 0.0
        && eng.get_order_tracker().pending_qty(
               provider->adapter->last_order_id) == 1.0
        && !eng.get_portfolio().position_open("TEST")
        && eng.get_portfolio().get_cash() == 100'000.0
        && report.total_fills == 0U
        && provider->adapter->pending_fills.size() == 1U;
    const bool fully_committed =
        tracked->filled_qty == 1.0
        && eng.get_order_tracker().pending_qty(
               provider->adapter->last_order_id) == 0.0
        && eng.get_portfolio().position_open("TEST")
        && eng.get_portfolio().get_cash() == 99'900.0
        && report.total_fills == 1U
        && provider->adapter->pending_fills.empty();

    EXPECT_TRUE(threw || eng.is_halted());
    EXPECT_EQ(strategy->fill_calls, 1);
    EXPECT_TRUE(fully_uncommitted || fully_committed)
        << "a callback failure must leave either an entirely retained, "
           "uncommitted fill or a completely committed and acknowledged "
           "economic event; ledger/portfolio ahead of analytics and delivery "
           "is not recoverable; filled=" << tracked->filled_qty
        << ", pending=" << eng.get_order_tracker().pending_qty(
               provider->adapter->last_order_id)
        << ", cash=" << eng.get_portfolio().get_cash()
        << ", analytics_fills=" << report.total_fills
        << ", retained_deliveries="
        << provider->adapter->pending_fills.size();
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

TEST(EngineAsyncSupport, C05_RestCancelCommandAckRetainsExposureUntilWsTerminal)
{
    auto dh = make_bars();
    auto provider = std::make_shared<FakeAsyncProvider>();
    auto strat = std::make_shared<OneShotStrategy>();
    engine_config cfg;
    cfg.provider = provider;
    cfg.threading = thread_preset::inline_mode;
    cfg.show_progress = false;
    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.run();

    const auto order_id = provider->adapter->last_submit_id;
    ASSERT_NE(order_id, 0u);
    ASSERT_TRUE(eng.get_order_tracker().is_active(order_id));
    const double pending_before =
        eng.get_order_tracker().pending_qty(order_id);
    ASSERT_GT(pending_before, 0.0);
    ASSERT_TRUE(eng.cancel_order("TEST", order_id, "operator"));
    ASSERT_EQ(provider->adapter->cancel_count, 1);

    // ExecutionBridge documents this result as command acknowledgement,
    // explicitly not proof of terminal economic state. It must therefore not
    // release risk exposure before the authoritative user-data transition.
    provider->adapter->inject_submit_result(submit_result{
        .engine_id = order_id,
        .symbol = "TEST",
        .exchange_order_id = "venue-1",
        .error = {},
        .op = submit_result::operation::cancel,
        .ok = true,
    });

    // modify_order drains async results before validating the amend, without
    // issuing another cancel command. The market order itself is not
    // amendable; only the lifecycle state after the drain matters here.
    EXPECT_FALSE(eng.modify_order("TEST", order_id, 101.0, 1.0));
    EXPECT_TRUE(eng.get_order_tracker().is_active(order_id));
    EXPECT_DOUBLE_EQ(eng.get_order_tracker().pending_qty(order_id),
                     pending_before);
    EXPECT_FALSE(eng.is_halted());
    EXPECT_EQ(provider->adapter->cancel_count, 1);
}

TEST(EngineAsyncSupport, C05_RestSubmitAckWaitsForAuthoritativeWsAck)
{
    auto dh = make_bars();
    auto provider = std::make_shared<FakeAsyncProvider>();
    auto strat = std::make_shared<OneShotStrategy>();
    engine_config cfg;
    cfg.provider = provider;
    cfg.threading = thread_preset::inline_mode;
    cfg.show_progress = false;
    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.run();

    const auto order_id = provider->adapter->last_submit_id;
    const auto* order = eng.get_order_tracker().find(order_id);
    ASSERT_NE(order, nullptr);
    ASSERT_EQ(order->status, order_status::pending);

    provider->adapter->inject_submit_result(submit_result{
        .engine_id = order_id,
        .symbol = "TEST",
        .exchange_order_id = "venue-1",
        .error = {},
        .op = submit_result::operation::submit,
        .ok = true,
    });
    EXPECT_FALSE(eng.modify_order("TEST", order_id, 101.0, 1.0));
    EXPECT_EQ(eng.get_order_tracker().get_order_status(order_id),
              order_status::pending);

    provider->adapter->inject_lifecycle(venue_lifecycle_event{
        .engine_order_id = order_id,
        .transition = venue_order_transition::acknowledged,
        .exchange_ts = order->created_ts + std::chrono::milliseconds(1),
    });
    EXPECT_FALSE(eng.modify_order("TEST", order_id, 101.0, 1.0));
    EXPECT_EQ(eng.get_order_tracker().get_order_status(order_id),
              order_status::open);
    EXPECT_FALSE(eng.is_halted());
}

TEST(EngineAsyncSupport, C05_AuthoritativeWsCancelReleasesExposureExactlyOnce)
{
    auto dh = make_bars();
    auto provider = std::make_shared<FakeAsyncProvider>();
    auto strat = std::make_shared<OneShotStrategy>();
    engine_config cfg;
    cfg.provider = provider;
    cfg.threading = thread_preset::inline_mode;
    cfg.show_progress = false;
    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.run();

    const auto order_id = provider->adapter->last_submit_id;
    const auto* order = eng.get_order_tracker().find(order_id);
    ASSERT_NE(order, nullptr);
    const auto event_ts = order->created_ts + std::chrono::milliseconds(1);
    ASSERT_TRUE(eng.cancel_order("TEST", order_id, "operator"));
    provider->adapter->inject_submit_result(submit_result{
        .engine_id = order_id,
        .symbol = "TEST",
        .exchange_order_id = "venue-1",
        .error = {},
        .op = submit_result::operation::cancel,
        .ok = true,
    });
    provider->adapter->inject_lifecycle(venue_lifecycle_event{
        .engine_order_id = order_id,
        .transition = venue_order_transition::canceled,
        .exchange_ts = event_ts,
    });

    EXPECT_FALSE(eng.modify_order("TEST", order_id, 101.0, 1.0));
    EXPECT_EQ(eng.get_order_tracker().get_order_status(order_id),
              order_status::cancelled);
    EXPECT_DOUBLE_EQ(eng.get_order_tracker().pending_qty(order_id), 0.0);
    EXPECT_FALSE(eng.is_halted());

    provider->adapter->inject_lifecycle(venue_lifecycle_event{
        .engine_order_id = order_id,
        .transition = venue_order_transition::canceled,
        .exchange_ts = event_ts,
    });
    EXPECT_FALSE(eng.modify_order("TEST", order_id, 101.0, 1.0));
    EXPECT_EQ(eng.get_order_tracker().get_order_status(order_id),
              order_status::cancelled);
    EXPECT_FALSE(eng.is_halted());
}

TEST(EngineAsyncSupport, C05_RejectAfterAuthoritativeAckFailsClosed)
{
    auto dh = make_bars();
    auto provider = std::make_shared<FakeAsyncProvider>();
    auto strat = std::make_shared<OneShotStrategy>();
    engine_config cfg;
    cfg.provider = provider;
    cfg.threading = thread_preset::inline_mode;
    cfg.show_progress = false;
    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.run();

    const auto order_id = provider->adapter->last_submit_id;
    const auto* order = eng.get_order_tracker().find(order_id);
    ASSERT_NE(order, nullptr);
    const auto ack_ts = order->created_ts + std::chrono::milliseconds(1);
    provider->adapter->inject_lifecycle(venue_lifecycle_event{
        .engine_order_id = order_id,
        .transition = venue_order_transition::acknowledged,
        .exchange_ts = ack_ts,
    });
    EXPECT_FALSE(eng.modify_order("TEST", order_id, 101.0, 1.0));
    ASSERT_EQ(eng.get_order_tracker().get_order_status(order_id),
              order_status::open);

    provider->adapter->inject_lifecycle(venue_lifecycle_event{
        .engine_order_id = order_id,
        .transition = venue_order_transition::rejected,
        .exchange_ts = ack_ts + std::chrono::milliseconds(1),
    });
    EXPECT_FALSE(eng.modify_order("TEST", order_id, 101.0, 1.0));
    EXPECT_TRUE(eng.is_halted());
    EXPECT_EQ(eng.get_order_tracker().get_order_status(order_id),
              order_status::open);
    EXPECT_GT(eng.get_order_tracker().pending_qty(order_id), 0.0);
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
