// Hybrid paper path through engine (backtest defect closure).
#include "helpers/backtest_defect_helpers.h"

TEST(BacktestDefects, FR01_EngineMakerQueue_MarketOrdersFill)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    load_bars(*dh, 30, 100.0);

    auto strat = std::make_shared<OneShotMarketBuy>();
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.show_progress = false;
    cfg.execution_bar_delay = 0; // fill same bar for a hard fill count
    cfg.initial_balance = 10000;
    cfg.maker_queue_model = std::make_shared<UniformCancelModel>();
    auto eng = std::make_unique<engine>(dh, nullptr, strat, std::move(cfg));
    eng->run();
    // Without Hybrid routing, market buy would silent-no-op → 0 fills.
    EXPECT_GE(eng->get_analytics().snapshot().total_fills, 1u);
}

// ── FR-02 hard fee: commissions must appear when fee model set ─────────────

TEST(BacktestDefects, HybridPaper_LimitFillsFromBarTradeTape)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    // Flat series so limit @ close matches subsequent bar trades.
    for (int i = 0; i < 20; ++i) {
        const long long ts = 1704067200000LL + static_cast<long long>(i) * 60000LL;
        dh->load_into_queue(std::to_string(ts), "BTCUSDT",
                            100.0, 100.5, 99.5, 100.0, 1000);
    }

    auto strat = std::make_shared<OneShotLimitAtClose>();
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.show_progress = false;
    cfg.execution_bar_delay = 0;
    cfg.initial_balance = 10000;
    cfg.maker_queue_model = std::make_shared<UniformCancelModel>();
    auto eng = std::make_unique<engine>(dh, nullptr, strat, std::move(cfg));
    eng->run();
    // Without paper on_trade feed, QueueAware limits never fill.
    EXPECT_GE(eng->get_analytics().snapshot().total_fills, 1u);
}

TEST(BacktestDefects, FixedPointBarVolumeIsBaseUnitsAndUsedOnlyOnce)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    const auto base = std::chrono::system_clock::time_point{
        std::chrono::seconds{1'700'000'000}};
    for (int i = 0; i < 2; ++i)
    {
        Bar bar;
        bar.ts = base + std::chrono::seconds{i};
        bar.symbol = "BTCUSDT";
        bar.open = 100.0;
        bar.high = 100.5;
        bar.low = 99.5;
        bar.close = 100.0;
        bar.volume = 300'000'000; // 3.0 base units
        bar.quantity_scale = 100'000'000ULL;
        ASSERT_TRUE(dh->on_bar(bar));
    }

    class TenUnitLimit final : public IStrategy
    {
        bool fired_ = false;
    public:
        std::optional<order_event> on_market(const market_event& m) override
        {
            if (fired_) return std::nullopt;
            fired_ = true;
            return order_event(m.get_timestamp(), m.get_symbol(),
                               order_type::limit, order_side::buy,
                               10.0, m.get_close());
        }
    };

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.show_progress = false;
    cfg.execution_bar_delay = 0;
    cfg.initial_balance = 10000;
    cfg.maker_queue_model = std::make_shared<UniformCancelModel>();
    cfg.exit_defaults.mode = truetest::exits::exit_policy_mode::strategy_only;

    engine eng(dh, nullptr, std::make_shared<TenUnitLimit>(), std::move(cfg));
    eng.run();

    const auto report = eng.get_analytics().generate_report();
    ASSERT_EQ(report.trades.size(), 1u);
    EXPECT_NEAR(report.trades.front().quantity, 3.0, 1e-9)
        << "raw fixed-point volume must be normalized and not reused by "
           "both range sweep and close tape";
}

// ── EL-HYBRID-SWEEP / FR-hybrid-bar-range-miss ─────────────────────────────
// Buy limit between open and close: bar low touches it but close misses.
// Hybrid + maker_queue must fill via QueueAware sweep_resting_range (not
// close-only exact-price tape).

TEST(BacktestDefects, HybridPaper_LimitFillsFromBarRangeSweep)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    // Bars: O=100 H=100.5 L=99 C=100 — limit@99.5 is in [L,H] but ≠ close.
    for (int i = 0; i < 20; ++i) {
        const long long ts = 1704067200000LL + static_cast<long long>(i) * 60000LL;
        dh->load_into_queue(std::to_string(ts), "BTCUSDT",
                            100.0, 100.5, 99.0, 100.0, 1000);
    }

    auto strat = std::make_shared<OneShotBuyLimitAt995>();
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.show_progress = false;
    cfg.execution_bar_delay = 0;
    cfg.initial_balance = 10000;
    cfg.maker_queue_model = std::make_shared<UniformCancelModel>();
    auto eng = std::make_unique<engine>(dh, nullptr, strat, std::move(cfg));
    eng->run();
    // Pre-fix: Hybrid sweep only hit LocalBook → 0 fills under maker_queue.
    EXPECT_GE(eng->get_analytics().snapshot().total_fills, 1u);
}

// Adapter-level lock: Hybrid demux must call queue sweep, not only local.
TEST(BacktestDefects, HybridDayLimit_EosCancelPathCompletes)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    load_bars(*dh, 15, 100.0);

    class DayFarLimit : public IStrategy {
        bool posted_ = false;
    public:
        std::optional<order_event> on_market(const market_event& m) override
        {
            if (posted_) return std::nullopt;
            posted_ = true;
            // Far below market — never trades; DAY should cancel via adapter at EOS.
            return order_event(m.get_timestamp(), m.get_symbol(),
                               order_type::limit, order_side::buy, 1.0, 1.0,
                               time_in_force::day);
        }
        void set_position_open(const std::string&, bool) override {}
    };

    auto strat = std::make_shared<DayFarLimit>();
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.show_progress = false;
    cfg.execution_bar_delay = 0;
    cfg.initial_balance = 10000;
    cfg.maker_queue_model = std::make_shared<UniformCancelModel>();
    auto eng = std::make_unique<engine>(dh, nullptr, strat, std::move(cfg));
    EXPECT_NO_THROW(eng->run());
    // Far limit never fills; cancel path must not leave engine stuck.
    EXPECT_EQ(eng->get_analytics().snapshot().total_fills, 0u);
    // HIGH-01: adapter-path EOS cancel must clear Hybrid/QueueAware residuals.
    EXPECT_EQ(eng->total_live_quotes(), 0u)
        << "DAY EOS cancel must leave no live queue/hybrid day orders";
}

// corr-5: cancel_day_orders must flush latency-deferred cancels so DAY
// residuals do not remain in live_quote_count after EOS.
TEST(BacktestDefects, HybridDayLimit_EosCancelWithLatencyClearsQuotes)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    load_bars(*dh, 15, 100.0);

    class DayFarLimit : public IStrategy {
        bool posted_ = false;
    public:
        std::optional<order_event> on_market(const market_event& m) override
        {
            if (posted_) return std::nullopt;
            posted_ = true;
            return order_event(m.get_timestamp(), m.get_symbol(),
                               order_type::limit, order_side::buy, 1.0, 1.0,
                               time_in_force::day);
        }
        void set_position_open(const std::string&, bool) override {}
    };

    auto strat = std::make_shared<DayFarLimit>();
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.show_progress = false;
    cfg.execution_bar_delay = 0;
    cfg.initial_balance = 10000;
    cfg.maker_queue_model = std::make_shared<UniformCancelModel>();
    // Large cancel latency relative to bar spacing — without EOS flush,
    // pending cancel would leave the order live.
    cfg.latency_model = std::make_shared<FixedLatencyModel>(
        latency_duration(0), latency_duration(0),
        std::chrono::duration_cast<latency_duration>(std::chrono::hours(24)));
    auto eng = std::make_unique<engine>(dh, nullptr, strat, std::move(cfg));
    EXPECT_NO_THROW(eng->run());
    EXPECT_EQ(eng->total_live_quotes(), 0u)
        << "DAY EOS cancel + latency flush must clear queue residuals";
}

// ── MEDIUM-02: soft post-fill counters reach export under threaded preset ──

TEST(BacktestDefects, EL_ReplayPaperTape_HybridLimitFillParity)
{
    SilenceOutput silence;
    const std::string log_path = "/tmp/truetest_replay_paper_tape.bin";
    std::remove(log_path.c_str());

    auto make_dh = []() {
        auto dh = std::make_shared<data_handler>();
        for (int i = 0; i < 20; ++i) {
            const long long ts = 1704067200000LL + static_cast<long long>(i) * 60000LL;
            dh->load_into_queue(std::to_string(ts), "BTCUSDT",
                                100.0, 100.5, 99.5, 100.0, 1000);
        }
        return dh;
    };

    int batch_fills = 0;
    {
        auto strat = std::make_shared<OneShotLimitAtCloseFillCount>();
        engine_config cfg;
        cfg.mode = engine_mode::backtest;
        cfg.seed = 7;
        cfg.show_progress = false;
        cfg.execution_bar_delay = 0;
        cfg.initial_balance = 10000;
        cfg.maker_queue_model = std::make_shared<UniformCancelModel>();
        cfg.event_log_path = log_path;
        cfg.exit_defaults.mode = truetest::exits::exit_policy_mode::strategy_only;
        engine eng(make_dh(), nullptr, strat, std::move(cfg));
        eng.run();
        batch_fills = strat->fill_callbacks;
    }
    ASSERT_GE(batch_fills, 1) << "batch paper tape must fill hybrid limit";

    std::size_t replay_fills = 0;
    {
        auto strat = std::make_shared<OneShotLimitAtCloseFillCount>();
        engine_config cfg;
        cfg.mode = engine_mode::backtest;
        cfg.seed = 7;
        cfg.show_progress = false;
        cfg.execution_bar_delay = 0;
        cfg.initial_balance = 10000;
        cfg.maker_queue_model = std::make_shared<UniformCancelModel>();
        cfg.exit_defaults.mode = truetest::exits::exit_policy_mode::strategy_only;
        engine eng(std::make_shared<data_handler>(), nullptr, strat, std::move(cfg));
        eng.run_replay(log_path);
        EXPECT_EQ(strat->fill_callbacks, 0)
            << "ledger replay must not invoke strategy callbacks";
        replay_fills = eng.get_analytics().snapshot().total_fills;
    }
    std::remove(log_path.c_str());

    EXPECT_EQ(replay_fills, static_cast<std::size_t>(batch_fills))
        << "recorded fills must be applied once without paper-tape regeneration";
}

// ── Deferred residual locks (2026-08-12 reliability repair) ────────────────
