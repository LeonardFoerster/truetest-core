// Engine event-loop / EOS / multi-symbol locks (backtest defect closure).
#include "helpers/backtest_defect_helpers.h"

TEST(BacktestDefects, EL01_TickPath_NoDualMarketDispatch)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    // 3 seconds of ticks at 1 per 100ms → a few 1s bars if aggregated.
    for (int i = 0; i < 30; ++i) {
        tick_record t;
        t.timestamp = t_at(i * 100);
        t.symbol = "BTCUSDT";
        t.price = 100.0 + i * 0.01;
        t.quantity = 1;
        t.side = data_tick_side::unknown;
        dh->add_tick(t);
    }

    auto strat = std::make_shared<DispatchCountStrategy>();
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.show_progress = false;
    cfg.initial_balance = 10000;
    auto eng = std::make_unique<engine>(dh, nullptr, strat, std::move(cfg));
    eng->run_tick_data();

    EXPECT_EQ(strat->tick_calls, 30);
    EXPECT_EQ(strat->market_calls, 0)
        << "tick path must not dual-dispatch on_market for the same strategy";
}

// ── FR-02: MC trials apply fee model ───────────────────────────────────────

TEST(BacktestDefects, EL03_BatchWithBarDelay_FinalPendingDrains)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    load_bars(*dh, 40, 100.0);

    auto strat = std::make_shared<OneShotMarketBuy>();
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.show_progress = false;
    cfg.execution_bar_delay = 1;
    cfg.initial_balance = 10000;
    auto eng = std::make_unique<engine>(dh, nullptr, strat, std::move(cfg));
    eng->run();
    // One-shot buy with delay=1 must still fill before EOS drain completes.
    EXPECT_GE(eng->get_analytics().snapshot().total_fills, 1u);
}

// ── DR-02: validation errors counted ───────────────────────────────────────

TEST(BacktestDefects, EL02_EngineBatch_LatencyModelAdvancesWithoutCrash)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    load_bars(*dh, 20, 100.0);

    auto strat = std::make_shared<DayLimitPoster>();
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.show_progress = false;
    cfg.execution_bar_delay = 0;
    cfg.initial_balance = 10000;
    cfg.latency_model = std::make_shared<FixedLatencyModel>(
        latency_duration(0), latency_duration(0),
        std::chrono::duration_cast<latency_duration>(50ms));
    auto eng = std::make_unique<engine>(dh, nullptr, strat, std::move(cfg));
    // Must complete: advance_all each bar drains cancel windows; EOS cancels DAY.
    EXPECT_NO_THROW(eng->run());
}

// ── EL-03 streaming EOS drains delayed market order ────────────────────────

TEST(BacktestDefects, EL03_StreamingEosDrainsDelayedOrder)
{
    SilenceOutput silence;
    auto transport = std::make_shared<MockStreamingTransport>();
    auto parser = std::make_shared<CsvBarParser>();
    auto bridge = std::make_shared<DataBridge<bar_record>>(
        transport, parser, bar_record_sink);

    auto dh = std::make_shared<data_handler>();
    auto strat = std::make_shared<OneShotMarketBuy>();

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.seed = 42;
    cfg.show_progress = false;
    cfg.execution_bar_delay = 1; // parks order off same-bar fill
    cfg.initial_balance = 10000;
    engine eng(dh, nullptr, strat, std::move(cfg));

    std::thread feeder([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        // Header + enough bars for delay=1 park + room to drain at EOS
        transport->enqueue("date,symbol,open,high,low,close,volume");
        for (int i = 0; i < 8; ++i) {
            const long long ts = 1704067200000LL + static_cast<long long>(i) * 60000LL;
            const double px = 100.0 + i;
            transport->enqueue(
                std::to_string(ts) + ",BTCUSDT," +
                std::to_string(px) + "," + std::to_string(px + 1) + "," +
                std::to_string(px - 1) + "," + std::to_string(px) + ",1000");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        transport->request_stop();
    });

    eng.run_streaming(bridge);
    feeder.join();

    // Without EOS drain_final_pending, delay=1 last/parked order is abandoned.
    EXPECT_GE(eng.get_analytics().snapshot().total_fills, 1u);
}

// ── EL-08 tick EOS cancels DAY residuals (no crash + clean end) ────────────

TEST(BacktestDefects, EL08_TickEos_DayLimitPathCompletes)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    for (int i = 0; i < 40; ++i) {
        tick_record t;
        t.timestamp = t_at(i * 100);
        t.symbol = "BTCUSDT";
        t.price = 100.0;
        t.quantity = 1;
        t.side = data_tick_side::unknown;
        dh->add_tick(t);
    }
    // Strategy that only implements on_tick: post one far DAY limit.
    class TickDayLimit : public IStrategy {
        bool posted_ = false;
    public:
        std::optional<order_event> on_market(const market_event&) override
        {
            return std::nullopt;
        }
        std::optional<order_event> on_tick(const tick_event& te) override
        {
            if (posted_) return std::nullopt;
            posted_ = true;
            return order_event(te.get_timestamp(), te.get_symbol(),
                               order_type::limit, order_side::buy, 1.0, 1.0,
                               time_in_force::day);
        }
        void set_position_open(const std::string&, bool) override {}
    };
    auto strat = std::make_shared<TickDayLimit>();
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.show_progress = false;
    cfg.initial_balance = 10000;
    auto eng = std::make_unique<engine>(dh, nullptr, strat, std::move(cfg));
    EXPECT_NO_THROW(eng->run_tick_data());
}

// ── FR-07 strategy fee-aware sizing ────────────────────────────────────────

TEST(BacktestDefects, EL06_SoftPostFillBreachCountedInReport)
{
    Analytics a(10000.0);
    EXPECT_EQ(a.snapshot().soft_post_fill_breaches, 0u);
    a.note_soft_post_fill_breach();
    a.note_soft_post_fill_breach();
    a.set_data_rows_rejected(3);
    auto r = a.snapshot();
    EXPECT_EQ(r.soft_post_fill_breaches, 2u);
    EXPECT_EQ(r.data_rows_rejected, 3u);
}

// ── HIGH-02: hybrid modify must not cancel-and-return-true ─────────────────

TEST(BacktestDefects, SoftPostFill_SurfacesInReportUnderStandardThreading)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    // Flat then dump: buy early, sell into lower prices → closed-trade loss
    // trips max_loss_per_trade (post-fill soft path — not pre-trade reject).
    for (int i = 0; i < 30; ++i) {
        const long long ts = 1704067200000LL + static_cast<long long>(i) * 60000LL;
        const double px = (i < 5) ? 100.0 : 90.0;
        dh->load_into_queue(std::to_string(ts), "BTCUSDT",
                            px, px + 0.5, px - 0.5, px, 1000);
    }

    class BuyThenSell : public IStrategy {
        int n_ = 0;
    public:
        std::optional<order_event> on_market(const market_event& m) override
        {
            ++n_;
            if (n_ == 1) {
                return order_event(m.get_timestamp(), m.get_symbol(),
                                   order_type::market, order_side::buy, 10.0, m.get_close());
            }
            if (n_ == 10) {
                return order_event(m.get_timestamp(), m.get_symbol(),
                                   order_type::market, order_side::sell, 10.0, m.get_close());
            }
            return std::nullopt;
        }
        void set_position_open(const std::string&, bool) override {}
    };

    auto strat = std::make_shared<BuyThenSell>();
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.show_progress = false;
    cfg.initial_balance = 10000;
    cfg.threading = thread_preset::standard;
    cfg.disable_pinning = true;
    cfg.risk_soft_portfolio_limits = true;
    cfg.exit_defaults.mode = truetest::exits::exit_policy_mode::strategy_only;
    cfg.exit_defaults.sl_pct = 0.0;
    cfg.exit_defaults.tp_pct = 0.0;
    // ~$100 loss on 10 units × $10 move — well above $1 cap.
    cfg.risk.max_loss_per_trade = 1.0;
    cfg.risk.max_drawdown = 0.99; // do not trip DD pre-trade reject
    auto eng = std::make_unique<engine>(dh, nullptr, strat, std::move(cfg));
    eng->run();

    // get_analytics() under standard returns worker analytics; soft counters
    // must be folded in at stop so report/export is honest.
    const auto rep = eng->get_analytics().generate_report();
    EXPECT_GE(rep.soft_post_fill_breaches, 1u)
        << "soft post-fill must appear on exported analytics under standard preset"
        << " fills=" << rep.total_fills
        << " trades=" << rep.total_trades;
}

// ── M3: SMA reset clears maps for MC reuse honesty ────────────────────────

TEST(BacktestDefects, FR06_MultiSymbolEquityUsesPerSymbolMarks)
{
    portfolio p(10000.0);
    // Long 1 unit A @ 100, long 1 unit B @ 200 via synthetic fills.
    fill_event fa(t0(), "A", 1, order_side::buy, 1.0, 100.0, 0.0);
    fill_event fb(t0(), "B", 2, order_side::buy, 1.0, 200.0, 0.0);
    p.on_fill(fa);
    p.on_fill(fb);
    // cash = 10000 - 100 - 200 = 9700
    std::unordered_map<std::string, double> marks{{"A", 110.0}, {"B", 180.0}};
    // equity = 9700 + 110 + 180 = 9990
    EXPECT_NEAR(p.get_equity(marks, 0.0), 9990.0, 1e-6);
    // Single-price path wrongly marks both at 110 → 9700+110+110=9920
    EXPECT_NEAR(p.get_equity(110.0), 9920.0, 1e-6);
}

// ── EL-03: streaming EOS drains pending (via process_single_bar + delay) ──

TEST(BacktestDefects, EL_MultiSym_PendingFillUsesOrderSymbolMid)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    // Interleaved BTC (~50k) and ETH (~3k). Buy BTC on first BTC bar;
    // with delay=1 the order drains on the next (ETH) bar — mid must stay BTC.
    const long long t0ms = 1704067200000LL;
    dh->load_into_queue(std::to_string(t0ms), "BTCUSDT",
                        50000.0, 50100.0, 49900.0, 50000.0, 100);
    dh->load_into_queue(std::to_string(t0ms + 60000), "ETHUSDT",
                        3000.0, 3010.0, 2990.0, 3000.0, 1000);
    dh->load_into_queue(std::to_string(t0ms + 120000), "BTCUSDT",
                        50000.0, 50100.0, 49900.0, 50000.0, 100);
    dh->load_into_queue(std::to_string(t0ms + 180000), "ETHUSDT",
                        3000.0, 3010.0, 2990.0, 3000.0, 1000);
    for (int i = 4; i < 12; ++i) {
        const bool btc = (i % 2 == 0);
        const char* sym = btc ? "BTCUSDT" : "ETHUSDT";
        const double px = btc ? 50000.0 : 3000.0;
        dh->load_into_queue(std::to_string(t0ms + static_cast<long long>(i) * 60000LL),
                            sym, px, px + 10, px - 10, px, 100);
    }

    class BuyBtcOnce : public IStrategy {
        bool done_ = false;
    public:
        std::optional<order_event> on_market(const market_event& m) override
        {
            if (done_ || m.get_symbol() != "BTCUSDT") return std::nullopt;
            done_ = true;
            return order_event(m.get_timestamp(), m.get_symbol(),
                               order_type::market, order_side::buy,
                               0.01, m.get_close());
        }
        void set_position_open(const std::string&, bool) override {}
    };

    auto strat = std::make_shared<BuyBtcOnce>();
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.seed = 1;
    cfg.show_progress = false;
    cfg.execution_bar_delay = 1;
    cfg.initial_balance = 100000;
    cfg.exit_defaults.mode = truetest::exits::exit_policy_mode::strategy_only;
    auto eng = std::make_unique<engine>(dh, nullptr, strat, std::move(cfg));
    eng->run();

    auto report = eng->get_analytics().generate_report();
    ASSERT_GE(report.trades.size(), 1u);
    const auto& t = report.trades.front();
    EXPECT_EQ(t.symbol, "BTCUSDT");
    // Without per-symbol mid, fill walks ETH open (~3k). With the fix it
    // tracks BTC marks (~50k).
    EXPECT_GT(t.fill_price, 10000.0)
        << "BTC delayed fill must not use ETH mid; fill_price=" << t.fill_price;
    EXPECT_NEAR(t.fill_price, 50000.0, 2500.0);
}

// ── EL-TICK-BAR-EXIT-LA: tick-path 1s bars must not look-ahead stop ───────

TEST(BacktestDefects, EL_TickBarExit_NoLookaheadOnSyntheticBar)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    // Second 0: adverse low 90 then recover to 100.
    // Second 1 first tick @100: buy + arm SL@99; prior bar emit must NOT
    // stop out on the pre-entry low.
    for (int i = 0; i < 10; ++i) {
        tick_record t;
        t.timestamp = t_at(i * 100); // 0..900 ms
        t.symbol = "BTCUSDT";
        t.price = (i == 0) ? 90.0 : 100.0;
        t.quantity = 1;
        t.side = data_tick_side::unknown;
        dh->add_tick(t);
    }
    for (int i = 0; i < 10; ++i) {
        tick_record t;
        t.timestamp = t_at(1000 + i * 100); // new 1s window
        t.symbol = "BTCUSDT";
        t.price = 100.0;
        t.quantity = 1;
        t.side = data_tick_side::unknown;
        dh->add_tick(t);
    }

    class BuyOnFirstSecondTick : public IStrategy {
        bool done_ = false;
    public:
        std::optional<order_event> on_market(const market_event&) override
        {
            return std::nullopt;
        }
        std::optional<order_event> on_tick(const tick_event& te) override
        {
            if (done_) return std::nullopt;
            // First tick of the second 1s window — bar_agg will emit prior OHLC
            // after this strategy callback when on_tick is called on the agg.
            if (te.get_timestamp() < t_at(1000)) return std::nullopt;
            done_ = true;
            return order_event(te.get_timestamp(), te.get_symbol(),
                               order_type::market, order_side::buy,
                               1.0, te.get_price());
        }
        void set_position_open(const std::string&, bool) override {}
    };

    auto strat = std::make_shared<BuyOnFirstSecondTick>();
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.seed = 1;
    cfg.show_progress = false;
    cfg.execution_bar_delay = 0; // fill on entry tick so SL arms before bar emit
    cfg.initial_balance = 100000;
    cfg.exit_defaults.mode = truetest::exits::exit_policy_mode::floor;
    cfg.exit_defaults.sl_pct = 0.01;  // SL @ 99 — prior bar low 90 would fire OHLC
    cfg.exit_defaults.tp_pct = 0.50;
    auto eng = std::make_unique<engine>(dh, nullptr, strat, std::move(cfg));
    eng->run_tick_data();

    auto report = eng->get_analytics().generate_report();
    ASSERT_GE(report.trades.size(), 1u);
    // Entry only — no look-ahead SL close on pre-entry low.
    std::size_t sells = 0;
    for (const auto& tr : report.trades)
        if (tr.side == order_side::sell) ++sells;
    EXPECT_EQ(sells, 0u)
        << "synthetic 1s bar OHLC must not stop out on pre-entry extremes";
}

// ── DR-REPLAY-01: streaming bars use open_time_ms / date_parse, not wall clock ─

TEST(BacktestDefects, EL_TickPath_NullStrategyNoCrash)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    for (int i = 0; i < 10; ++i) {
        tick_record t;
        t.timestamp = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(1704067200000LL + i * 100));
        t.symbol = "BTCUSDT";
        t.price = 100.0 + i * 0.01;
        t.quantity = 1;
        t.side = data_tick_side::unknown;
        dh->add_tick(t);
    }
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.show_progress = false;
    cfg.initial_balance = 10000;
    // Null strategy — must not deref (EL-TICK-NULL-STRATEGY).
    engine eng(dh, nullptr, nullptr, std::move(cfg));
    EXPECT_NO_THROW(eng.run_tick_data());
}

TEST(BacktestDefects, EL_CancelUsesSimTimeNotWallClock)
{
    SilenceOutput silence;
    // Resting DAY limit cancelled at EOS must stamp sim time, not wall clock.
    class LimitThenHold : public IStrategy {
        bool done_ = false;
    public:
        std::optional<order_event> on_market(const market_event& m) override
        {
            if (done_) return std::nullopt;
            done_ = true;
            return order_event(m.get_timestamp(), m.get_symbol(),
                               order_type::limit, order_side::buy, 1.0, 50.0,
                               time_in_force::day);
        }
        void set_position_open(const std::string&, bool) override {}
    };

    auto dh = std::make_shared<data_handler>();
    for (int i = 0; i < 5; ++i) {
        const long long ts = 1704067200000LL + static_cast<long long>(i) * 60000LL;
        dh->load_into_queue(std::to_string(ts), "BTCUSDT",
                            100.0, 100.0, 100.0, 100.0, 100);
    }
    auto strat = std::make_shared<LimitThenHold>();
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.show_progress = false;
    cfg.initial_balance = 10000;
    cfg.execution_bar_delay = 0;
    // Limit far below never fills → DAY cancel at EOS.
    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.run();
    // Cancel path must run with last_sim_time_ (not wall clock); EOS clears quotes.
    EXPECT_EQ(eng.total_live_quotes(), 0u);
}
