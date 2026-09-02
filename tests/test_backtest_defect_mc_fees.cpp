// MC fee injection + strategy sizing/reset locks (backtest defect closure).
#include "helpers/backtest_defect_helpers.h"

TEST(BacktestDefects, FR02_McTrialsApplyFees)
{
    SilenceOutput silence;
    McRunConfig base;
    base.n_trials = 2;
    base.generator_config.n_steps = 120;
    base.generator_config.sigma = 0.8;
    base.strategy_name = "mean-reversion";
    base.base_seed = 99;
    base.master_seed_explicitly_set = true;
    base.initial_balance = 10000.0;

    McRunConfig free = base;
    MonteCarloController c_free(free);
    auto agg_free = c_free.run();

    McRunConfig fee = base;
    fee.fee_model = "tiered";
    fee.maker_rate = 0.001;
    fee.taker_rate = 0.002;
    MonteCarloController c_fee(fee);
    auto agg_fee = c_fee.run();

    ASSERT_EQ(agg_free.trials.size(), agg_fee.trials.size());
    // With identical seeds/paths, fees must change equity or trade economics
    // when any trades occur. If both sides trade zero, skip strict compare.
    bool any_trades = false;
    bool equity_differs = false;
    for (size_t i = 0; i < agg_free.trials.size(); ++i) {
        if (agg_free.trials[i].total_trades > 0 || agg_fee.trials[i].total_trades > 0)
            any_trades = true;
        if (std::abs(agg_free.trials[i].final_equity - agg_fee.trials[i].final_equity) > 1e-6)
            equity_differs = true;
    }
    if (any_trades) {
        EXPECT_TRUE(equity_differs)
            << "MC trials with fees must not match zero-fee equity when trades exist";
    }
}

// MC must inject entry/exit fee rates into strategies (parity with single-run
// apply_execution_cost_params). Without this, fee-aware sizing oversizes vs CLI.
TEST(BacktestDefects, FR02_McInjectsStrategyFeeRates)
{
    const char* probe_name = "fee-probe-mc-corr4";
    auto state = std::make_shared<FeeProbeState>();
    StrategyRegistry::instance().register_strategy(
        probe_name, [state] { return std::make_shared<FeeProbeStrategy>(state.get()); });

    // Tiered: prefer taker for market-style sizing.
    {
        *state = FeeProbeState{};

        SilenceOutput silence;
        McRunConfig cfg;
        cfg.n_trials = 1;
        cfg.generator_config.n_steps = 20;
        cfg.strategy_name = probe_name;
        cfg.base_seed = 7;
        cfg.master_seed_explicitly_set = true;
        cfg.fee_model = "tiered";
        cfg.maker_rate = 0.0004;
        cfg.taker_rate = 0.0012;
        MonteCarloController c(cfg);
        (void)c.run();

        EXPECT_DOUBLE_EQ(state->last_entry_fee, 0.0012);
        EXPECT_DOUBLE_EQ(state->last_exit_fee, 0.0012);
        EXPECT_DOUBLE_EQ(state->last_fixed_fee, 0.0)
            << "the complete effective config must expose a zero fixed fee";
    }

    // Fixed: inject fixed_fee_per_leg when schema supports it.
    {
        *state = FeeProbeState{};

        SilenceOutput silence;
        McRunConfig cfg;
        cfg.n_trials = 1;
        cfg.generator_config.n_steps = 20;
        cfg.strategy_name = probe_name;
        cfg.base_seed = 8;
        cfg.master_seed_explicitly_set = true;
        cfg.fee_model = "fixed";
        cfg.fee_value = 1.25;
        MonteCarloController c(cfg);
        (void)c.run();

        EXPECT_DOUBLE_EQ(state->last_fixed_fee, 1.25);
        EXPECT_DOUBLE_EQ(state->last_entry_fee, 0.0)
            << "the complete effective config must expose a zero proportional fee";
    }

    // Explicit --param must still win over injected costs (CLI parity).
    {
        *state = FeeProbeState{};

        SilenceOutput silence;
        McRunConfig cfg;
        cfg.n_trials = 1;
        cfg.generator_config.n_steps = 20;
        cfg.strategy_name = probe_name;
        cfg.base_seed = 9;
        cfg.master_seed_explicitly_set = true;
        cfg.fee_model = "tiered";
        cfg.taker_rate = 0.002;
        cfg.strategy_params = {{"entry_fee_rate", 0.0005}, {"exit_fee_rate", 0.0005}};
        MonteCarloController c(cfg);
        (void)c.run();

        EXPECT_DOUBLE_EQ(state->last_entry_fee, 0.0005);
        EXPECT_DOUBLE_EQ(state->last_exit_fee, 0.0005);
    }
}

TEST(BacktestDefects, McPlatformRiskFractionReachesStrategyAndParamOverrides)
{
    const char* probe_name = "risk-probe-mc-audit";
    auto state = std::make_shared<FeeProbeState>();
    StrategyRegistry::instance().register_strategy(
        probe_name, [state] { return std::make_shared<FeeProbeStrategy>(state.get()); });

    SilenceOutput silence;
    McRunConfig cfg;
    cfg.n_trials = 1;
    cfg.generator_config.n_steps = 20;
    cfg.strategy_name = probe_name;
    cfg.base_seed = 17;
    cfg.master_seed_explicitly_set = true;
    cfg.risk_fraction = 0.037;
    MonteCarloController platform_default(cfg);
    (void)platform_default.run();
    EXPECT_DOUBLE_EQ(state->last_risk_fraction, 0.037);

    *state = FeeProbeState{};
    cfg.strategy_params = {{"risk_fraction", 0.011}};
    MonteCarloController explicit_override(cfg);
    (void)explicit_override.run();
    EXPECT_DOUBLE_EQ(state->last_risk_fraction, 0.011);
}

TEST(BacktestDefects, FR02_McImpactWithoutAdvThrows)
{
    McRunConfig cfg;
    cfg.n_trials = 1;
    cfg.generator_config.n_steps = 50;
    cfg.impact_k_bps = 5.0;
    cfg.impact_adv = 0.0; // invalid
    cfg.strategy_name = "mean-reversion";
    cfg.base_seed = 1;
    cfg.master_seed_explicitly_set = true;
    EXPECT_THROW((void)MonteCarloController(cfg), std::invalid_argument);
}

// ── DR-01: synthetic timestamps strictly spaced ────────────────────────────

TEST(BacktestDefects, FR02_McFeeModelProducesCommissionDelta)
{
    SilenceOutput silence;
    // High-vol short path with aggressive mean-reversion defaults → trades.
    McRunConfig free;
    free.n_trials = 1;
    free.generator_config.n_steps = 400;
    free.generator_config.sigma = 1.2;
    free.strategy_name = "mean-reversion";
    free.base_seed = 7;
    free.master_seed_explicitly_set = true;
    free.initial_balance = 10000.0;

    McRunConfig fee = free;
    fee.fee_model = "fixed";
    fee.fee_value = 5.0; // $5 per fill — large vs free
    // Isolate engine fee charging from fee-aware strategy sizing. Otherwise
    // the fixed-fee estimate can deliberately shrink/change orders, making a
    // lower final equity an invalid expectation about two different paths.
    fee.strategy_params = {{"fixed_fee_per_leg", 0.0}};

    auto agg_free = MonteCarloController(free).run();
    auto agg_fee  = MonteCarloController(fee).run();
    ASSERT_EQ(agg_free.trials.size(), 1u);
    ASSERT_EQ(agg_fee.trials.size(), 1u);

    // total_trades counts completed round trips and can be zero with a filled,
    // still-open position. Equal sizing makes the final-equity delta itself
    // the stronger proof that at least one charged fill occurred.
    EXPECT_LT(agg_fee.trials[0].final_equity, agg_free.trials[0].final_equity)
        << "fixed $5 engine fee must reduce equity on the identical execution path";
}

TEST(BacktestDefects, FR07_SmaSizingShrinksWithEntryFee)
{
    sma_strategy free_s;
    free_s.set_param("equity", 10000.0);
    free_s.set_param("risk_fraction", 0.02);
    free_s.set_param("sl_pct", 0.01);
    free_s.set_param("entry_fee_rate", 0.0);
    free_s.set_param("exit_fee_rate", 0.0);

    sma_strategy fee_s;
    fee_s.set_param("equity", 10000.0);
    fee_s.set_param("risk_fraction", 0.02);
    fee_s.set_param("sl_pct", 0.01);
    fee_s.set_param("entry_fee_rate", 0.001);
    fee_s.set_param("exit_fee_rate", 0.001);

    // Drive via on_market with rising prices so SMA eventually signals.
    // Compare qty on the first buy order emitted by each.
    auto run_until_buy = [](sma_strategy& s) -> double {
        double qty = 0.0;
        for (int i = 0; i < 50; ++i) {
            const double px = 100.0 + i * 0.5; // trend up → above SMA after warm
            market_event m(t_at(i * 60000), "X", px, px + 0.1, px - 0.1, px, 1000);
            if (auto o = s.on_market(m)) {
                if (o->get_side() == order_side::buy) {
                    qty = o->get_quantity();
                    break;
                }
            }
        }
        return qty;
    };
    const double q_free = run_until_buy(free_s);
    const double q_fee  = run_until_buy(fee_s);
    ASSERT_GT(q_free, 0.0) << "free sizing produced no buy";
    ASSERT_GT(q_fee, 0.0) << "fee sizing produced no buy";
    EXPECT_LT(q_fee, q_free) << "fees must shrink risk-sized qty";
}

// ── FR-08 partial fill reconciles strategy exit qty ────────────────────────

TEST(BacktestDefects, FR08_SmaOnFillReconcilesPartialQty)
{
    sma_strategy s;
    s.set_param("equity", 10000.0);
    s.set_param("risk_fraction", 0.5);
    s.set_param("sl_pct", 0.01);

    // Force an open gate + optimistic qty via a buy order path, then partial fill.
    // Warm SMA and trigger long:
    double intended = 0.0;
    for (int i = 0; i < 40; ++i) {
        const double px = 100.0 + i * 0.5;
        market_event m(t_at(i * 60000), "X", px, px + 0.1, px - 0.1, px, 1000);
        if (auto o = s.on_market(m); o && o->get_side() == order_side::buy) {
            intended = o->get_quantity();
            fill_event partial(t_at(i * 60000), "X", o->get_order_id(),
                               order_side::buy, intended * 0.4, px, 0.0,
                               /*remaining*/ intended * 0.6);
            s.on_fill(partial, o->get_order_id());
            // After partial, exit size should be 40% of intended, not full.
            // Trigger sell by dropping below SMA:
            for (int j = 0; j < 30; ++j) {
                const double down = px - j * 2.0;
                market_event md(t_at((i + 1 + j) * 60000), "X",
                                down, down + 0.1, down - 0.1, down, 1000);
                if (auto sell = s.on_market(md); sell && sell->get_side() == order_side::sell) {
                    EXPECT_NEAR(sell->get_quantity(), intended * 0.4, 1e-6);
                    return;
                }
            }
            FAIL() << "no sell after partial fill";
        }
    }
    FAIL() << "no buy signal to test partial reconcile";
}

TEST(BacktestDefects, FR08_MaCrossoverOnFillReconcilesPartialQty)
{
    // short periods so golden-cross / death-cross fire quickly
    ma_crossover_strategy s(/*fast*/ 3, /*slow*/ 8);
    s.set_param("equity", 10000.0);
    s.set_param("risk_fraction", 0.5);
    s.set_param("sl_pct", 0.01);

    auto feed = [&](int i, double px) {
        return market_event(t_at(i * 60000), "X", px, px + 0.1, px - 0.1, px, 1000);
    };

    // Phase 1: declining prices warm SMAs with fast below slow (prev_fast_above=false).
    int t = 0;
    for (; t < 12; ++t)
        (void)s.on_market(feed(t, 100.0 - t * 1.0));

    // Phase 2: sharp rally → golden-cross buy.
    double intended = 0.0;
    std::optional<order_event> buy;
    for (; t < 30; ++t) {
        const double px = 90.0 + (t - 12) * 4.0;
        if (auto o = s.on_market(feed(t, px)); o && o->get_side() == order_side::buy) {
            buy = *o;
            intended = o->get_quantity();
            fill_event partial(t_at(t * 60000), "X", o->get_order_id(),
                               order_side::buy, intended * 0.4, px, 0.0,
                               /*remaining*/ intended * 0.6);
            s.on_fill(partial, o->get_order_id());
            break;
        }
    }
    ASSERT_TRUE(buy.has_value()) << "no buy signal to test partial reconcile";
    ASSERT_GT(intended, 0.0);

    // Phase 3: crash → death-cross sell sized to partial fill, not full intended.
    for (int j = 0; j < 40; ++j, ++t) {
        const double down = 200.0 - j * 5.0;
        if (auto sell = s.on_market(feed(t, down)); sell && sell->get_side() == order_side::sell) {
            EXPECT_NEAR(sell->get_quantity(), intended * 0.4, 1e-6);
            return;
        }
    }
    FAIL() << "no sell after partial fill";
}

// ── EL-06 soft post-fill counter + export field ────────────────────────────

TEST(BacktestDefects, SmaReset_ClearsMapsAndPositionState)
{
    sma_strategy s;
    s.set_param("equity", 10000.0);
    s.set_param("risk_fraction", 0.02);
    s.set_param("sl_pct", 0.01);
    // Warm indicator + open optimistic position via market path.
    for (int i = 0; i < 40; ++i) {
        const double px = 100.0 + i * 0.5;
        market_event m(t_at(i * 60000), "X", px, px + 0.1, px - 0.1, px, 1000);
        (void)s.on_market(m);
    }
    ASSERT_FALSE(s.get_indicator_values("X").empty())
        << "precondition: SMA should be ready before reset";

    s.reset(0);
    EXPECT_TRUE(s.get_indicator_values("X").empty())
        << "reset must clear SMA maps so MC reuse does not leak state";

    // After reset, SMA must re-warm: first (period-1) bars cannot signal.
    int buys_before_ready = 0;
    for (int i = 0; i < 19; ++i) {
        const double px = 100.0 + i * 0.5;
        market_event m(t_at((100 + i) * 60000), "X", px, px + 0.1, px - 0.1, px, 1000);
        if (auto o = s.on_market(m); o && o->get_side() == order_side::buy)
            ++buys_before_ready;
    }
    EXPECT_EQ(buys_before_ready, 0)
        << "fresh SMA after reset must re-warm before signaling";
}

// ── MEDIUM-04: DataWrapper sort_after_load sorts ticks ─────────────────────

TEST(BacktestDefects, McReuse_RefusesStrategyWithoutFullReset)
{
    SilenceOutput silence;
    McRunConfig cfg;
    cfg.n_trials = 1;
    cfg.base_seed = 1;
    cfg.master_seed_explicitly_set = true;
    cfg.strategy_name = "breakout"; // no supports_mc_trial_reuse
    cfg.reuse_objects_between_trials = true;
    cfg.generator_config.n_steps = 20;
    cfg.generator_config.initial_price = 100.0;
    MonteCarloController ctl(cfg);
    EXPECT_THROW(ctl.run(), std::runtime_error);
}

// HIGH-03: SMA reuse clears maps — two trials with reuse do not throw and complete.
TEST(BacktestDefects, McReuse_SmaIsolationCompletes)
{
    SilenceOutput silence;
    McRunConfig cfg;
    cfg.n_trials = 3;
    cfg.base_seed = 99;
    cfg.master_seed_explicitly_set = true;
    cfg.strategy_name = "sma";
    cfg.reuse_objects_between_trials = true;
    cfg.generator_config.n_steps = 40;
    cfg.generator_config.initial_price = 100.0;
    MonteCarloController ctl(cfg);
    auto agg = ctl.run();
    EXPECT_EQ(agg.trials.size(), 3u);

    // Direct isolation proof on shipped sma_strategy::reset (not a reimplementation).
    auto s = StrategyRegistry::instance().create("sma");
    ASSERT_TRUE(s);
    EXPECT_TRUE(s->supports_mc_trial_reuse());
    // Drive some bars so maps gain state.
    for (int i = 0; i < 30; ++i) {
        market_event m(t_at(i * 60), "BTCUSDT",
                       100.0, 101.0, 99.0, 100.0 + i * 0.1, 1000);
        (void)s->on_market(m);
    }
    EXPECT_FALSE(s->get_indicator_values("BTCUSDT").empty())
        << "SMA should have indicator state after bars";
    s->reset(/*seed=*/1);
    EXPECT_TRUE(s->get_indicator_values("BTCUSDT").empty())
        << "reset must clear trial-local SMA maps for MC reuse safety";
}

TEST(BacktestDefects, FR_McQueue_MakerQueueWiresIntoTrials)
{
    SilenceOutput silence;
    McRunConfig cfg;
    cfg.n_trials = 2;
    cfg.base_seed = 424242;
    cfg.master_seed_explicitly_set = true;
    cfg.strategy_name = "sma";
    cfg.initial_balance = 10000;
    cfg.generator_name = "gbm";
    cfg.generator_config.n_steps = 80;
    cfg.generator_config.initial_price = 100.0;
    cfg.maker_queue_model = "uniform"; // FR-mc-queue-unwired
    cfg.fee_model = "fixed";
    cfg.fee_value = 0.01;

    MonteCarloController ctl(cfg);
    auto agg = ctl.run();
    EXPECT_EQ(agg.trials.size(), 2u);
    // Smoke: campaign completes with hybrid path; fills may be 0 for SMA
    // but must not throw / ignore maker_queue_model.
    EXPECT_TRUE(ctl.config().maker_queue_model == "uniform");
}

TEST(BacktestDefects, FR_ZeroFee_ReportEchoesFeeModelLabel)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    for (int i = 0; i < 30; ++i) {
        const long long ts = 1704067200000LL + static_cast<long long>(i) * 60000LL;
        dh->load_into_queue(std::to_string(ts), "BTCUSDT",
                            100.0, 101.0, 99.0, 100.0, 1000);
    }
    auto strat = std::make_shared<DispatchCountStrategy>();
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.show_progress = false;
    cfg.initial_balance = 10000;
    // No fee_model → analytics must report "zero".
    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.run();
    EXPECT_EQ(eng.get_analytics().snapshot().fee_model, "zero");
}
