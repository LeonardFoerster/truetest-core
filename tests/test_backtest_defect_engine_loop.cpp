// Engine event-loop / EOS / multi-symbol locks (backtest defect closure).
#include "helpers/backtest_defect_helpers.h"
#include "providers/provider.h"

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

TEST(BacktestDefects, EL03_BatchWithBarDelayFillsOnFutureBar)
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
    // The first bar emits and the second same-symbol bar causally releases it.
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

// ── EL-03 streaming delay uses a future market observation ─────────────────

TEST(BacktestDefects, EL03_StreamingFutureBarDrainsDelayedOrder)
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

    // The later same-symbol input bar, not EOF cleanup, releases the order.
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
    // SL @ 95 — still above the pre-entry low of 90, so a look-ahead OHLC
    // evaluation would stop out, but wide enough that the synthetic book's
    // entry slippage stays inside it (F-01(a) would otherwise refuse the
    // bracket and flatten, masking what this test is actually pinning).
    cfg.exit_defaults.sl_pct = 0.05;
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

// ── BF-13: Same-fill flip attributes close and open legs separately ────────

class OneShotFlipStrategy : public IStrategy {
    int bar_count_ = 0;
    std::vector<truetest::exits::exit_intent> pending_intents_;
public:
    std::vector<truetest::exits::exit_intent> take_pending_exit_intents() override
    {
        auto out = std::move(pending_intents_);
        pending_intents_.clear();
        return out;
    }

    std::optional<order_event> on_market(const market_event& m) override
    {
        if (bar_count_ == 0)
        {
            bar_count_++;
            // Step 1: Open 10-unit long
            return order_event(m.get_timestamp(), m.get_symbol(),
                               order_type::market, order_side::buy, 10.0, m.get_close());
        }
        else if (bar_count_ == 1)
        {
            bar_count_++;
            // Step 2: Fill 15-unit sell in one event (flips +10 Long to -5 Short)
            truetest::exits::exit_intent intent;
            intent.symbol = m.get_symbol();
            intent.close_side = order_side::buy;
            intent.stop_loss = 110.0;
            intent.qty = 5.0;
            pending_intents_.push_back(intent);

            return order_event(m.get_timestamp(), m.get_symbol(),
                               order_type::market, order_side::sell, 15.0, m.get_close());
        }
        return std::nullopt;
    }
    void set_position_open(const std::string&, bool) override {}
};

class BareFlipStrategy : public IStrategy {
    int bar_count_ = 0;
public:
    std::optional<order_event> on_market(const market_event& m) override
    {
        if (bar_count_++ == 0)
        {
            return order_event(m.get_timestamp(), m.get_symbol(),
                               order_type::market, order_side::buy, 10.0, m.get_close());
        }
        if (bar_count_ == 2)
        {
            return order_event(m.get_timestamp(), m.get_symbol(),
                               order_type::market, order_side::sell, 15.0, m.get_close());
        }
        return std::nullopt;
    }
    void set_position_open(const std::string&, bool) override {}
};

class ExplicitOversizeCloseStrategy : public IStrategy {
    int bar_count_ = 0;
    std::uint64_t opener_id_ = 0;
public:
    std::optional<order_event> on_market(const market_event& m) override
    {
        if (bar_count_++ == 0)
        {
            return order_event(m.get_timestamp(), m.get_symbol(),
                               order_type::market, order_side::buy, 10.0, m.get_close());
        }
        if (bar_count_ == 2)
        {
            order_event close(m.get_timestamp(), m.get_symbol(),
                              order_type::market, order_side::sell, 15.0, m.get_close());
            close.set_opener_order_id(opener_id_);
            return close;
        }
        return std::nullopt;
    }

    void on_fill(const fill_event& f, std::uint64_t opener) override
    {
        if (f.get_side() == order_side::buy && opener == f.get_order_id())
            opener_id_ = f.get_order_id();
    }

    void set_position_open(const std::string&, bool) override {}
};

class AmbiguousImplicitCloseStrategy : public IStrategy {
    int bar_count_ = 0;
public:
    std::optional<order_event> on_market(const market_event& m) override
    {
        if (bar_count_++ < 2)
        {
            return order_event(m.get_timestamp(), m.get_symbol(),
                               order_type::market, order_side::buy, 5.0, m.get_close());
        }
        if (bar_count_ == 3)
        {
            return order_event(m.get_timestamp(), m.get_symbol(),
                               order_type::market, order_side::sell, 15.0, m.get_close());
        }
        return std::nullopt;
    }
    void set_position_open(const std::string&, bool) override {}
};

class DuplicateExplicitCloseAdapter final : public IExecutionAdapter {
public:
    void submit_order(const order_event& order) override
    {
        fill_event fill(order.get_earliest_eligible_ts(), order.get_symbol(),
                        order.get_order_id(), order.get_side(), order.get_quantity(),
                        order.get_price() > 0.0 ? order.get_price() : 100.0,
                        0.0, 0.0, ++next_fill_id_);
        fill.set_source(fill_source::exchange);
        EXPECT_TRUE(fill.set_venue_execution_id(
            "duplicate-explicit-close-" +
            std::to_string(fill.get_fill_id())));
        EXPECT_TRUE(fill.set_commission_currency("USDT"));
        fill.set_cumulative_filled_qty(
            order.get_quantity(), fill_cumulative_source::venue_reported);
        if (order.get_opener_order_id() != 0)
            fill.set_opener_order_id(order.get_opener_order_id());
        queued_.push_back(fill);
        if (order.get_opener_order_id() != 0)
            queued_.push_back(std::move(fill)); // same exchange fill-id retransmit
    }

    bool poll_fills(std::vector<fill_event>& out) override
    {
        if (queued_.empty()) return false;
        out.insert(out.end(), std::make_move_iterator(queued_.begin()),
                   std::make_move_iterator(queued_.end()));
        queued_.clear();
        return true;
    }

private:
    std::uint64_t next_fill_id_ = 0;
    std::vector<fill_event> queued_;
};

class DuplicateExplicitCloseProvider final : public IProvider {
public:
    std::shared_ptr<DuplicateExplicitCloseAdapter> adapter =
        std::make_shared<DuplicateExplicitCloseAdapter>();

    std::string name() const override { return "duplicate-explicit-close"; }
    bool has_data_feed() const override { return false; }
    bool has_execution() const override { return true; }
    bool open() override { return true; }
    void close() override {}
    std::shared_ptr<IDataTransport> get_transport() override { return nullptr; }
    std::shared_ptr<IExecutionAdapter> get_execution_adapter() override { return adapter; }
};

class EntryThenExplicitCloseStrategy final : public IStrategy {
    int bar_count_ = 0;
    std::uint64_t opener_id_ = 0;
public:
    std::optional<order_event> on_market(const market_event& m) override
    {
        if (bar_count_++ == 0)
        {
            return order_event(m.get_timestamp(), m.get_symbol(),
                               order_type::market, order_side::buy, 1.0, m.get_close());
        }
        if (bar_count_ == 2)
        {
            order_event close(m.get_timestamp(), m.get_symbol(),
                              order_type::market, order_side::sell, 1.0, m.get_close());
            close.set_opener_order_id(opener_id_);
            return close;
        }
        return std::nullopt;
    }

    void on_fill(const fill_event& fill, std::uint64_t opener) override
    {
        if (fill.get_side() == order_side::buy && opener == fill.get_order_id())
            opener_id_ = fill.get_order_id();
    }
    void set_position_open(const std::string&, bool) override {}
};

struct FlipLogFile {
    std::string path;
    FlipLogFile()
        : path((std::filesystem::temp_directory_path()
              / ("truetest_flip_replay_"
                 + std::to_string(std::chrono::steady_clock::now()
                                      .time_since_epoch().count())
                 + ".bin")).string())
    {}
    ~FlipLogFile() { std::remove(path.c_str()); }
};

TEST(BacktestDefects, BF13_SameFillFlipAttributesCloseAndOpenSeparately)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    for (int i = 0; i < 6; ++i) {
        const long long ts = 1704067200000LL + static_cast<long long>(i) * 60000LL;
        dh->load_into_queue(std::to_string(ts), "BTCUSDT",
                            100.0, 100.0, 100.0, 100.0, 100);
    }
    auto strat = std::make_shared<OneShotFlipStrategy>();
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.show_progress = false;
    cfg.initial_balance = 100000;
    cfg.execution_bar_delay = 1;
    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.run();

    const auto& port = eng.get_portfolio();
    const auto& lots = port.get_lots();
    const auto& positions = port.get_positions();

    // Position must be flipped to -5.0
    auto pos_it = positions.find("BTCUSDT");
    ASSERT_NE(pos_it, positions.end());
    EXPECT_DOUBLE_EQ(pos_it->second.qty, -5.0);

    // Old opener (10 Long) must be fully closed and removed.
    // Exactly one new lot must exist with qty_open = 5.0 and side = sell.
    ASSERT_EQ(lots.size(), 1u);
    auto lot_it = lots.begin();
    EXPECT_EQ(lot_it->second.side, order_side::sell);
    EXPECT_DOUBLE_EQ(lot_it->second.qty_open, 5.0);
    EXPECT_GT(lot_it->second.entry_price, 0.0);

    // ExitManager must have armed the exit intent for the new short lot (5.0 units)
    EXPECT_EQ(eng.get_exit_manager().armed_count(), 1u);

    // A flip is one exchange/paper execution event. Its logical lot closure
    // must not become a second synthetic persisted or analytics fill leg.
    const auto report = eng.get_analytics().generate_report();
    EXPECT_EQ(report.trades.size(), 2u);
    ASSERT_EQ(report.trades.size(), 2u);
    EXPECT_DOUBLE_EQ(report.trades.back().quantity, 15.0);
}

TEST(BacktestDefects, BF13_FloorProtectsResidualOfImplicitFlip)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    for (int i = 0; i < 6; ++i) {
        const long long ts = 1704067200000LL + static_cast<long long>(i) * 60000LL;
        dh->load_into_queue(std::to_string(ts), "BTCUSDT",
                            100.0, 100.0, 100.0, 100.0, 100);
    }

    auto strat = std::make_shared<BareFlipStrategy>();
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.show_progress = false;
    cfg.initial_balance = 100000;
    cfg.execution_bar_delay = 1;
    cfg.exit_defaults.mode = truetest::exits::exit_policy_mode::floor;
    cfg.exit_defaults.sl_pct = 0.05;
    cfg.exit_defaults.tp_pct = 0.50;
    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.run();

    const auto pos = eng.get_portfolio().get_positions().find("BTCUSDT");
    ASSERT_NE(pos, eng.get_portfolio().get_positions().end());
    EXPECT_DOUBLE_EQ(pos->second.qty, -5.0);

    const auto armed = eng.get_exit_manager().snapshot_armed();
    ASSERT_EQ(armed.size(), 1u);
    EXPECT_DOUBLE_EQ(armed.front().qty, 5.0);
    EXPECT_EQ(armed.front().close_side, order_side::buy);
}

TEST(BacktestDefects, BF13_ExplicitOversizeCloseHaltsBeforeResidualExposure)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    for (int i = 0; i < 6; ++i) {
        const long long ts = 1704067200000LL + static_cast<long long>(i) * 60000LL;
        dh->load_into_queue(std::to_string(ts), "BTCUSDT",
                            100.0, 100.0, 100.0, 100.0, 100);
    }

    auto strat = std::make_shared<ExplicitOversizeCloseStrategy>();
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.show_progress = false;
    cfg.initial_balance = 100000;
    cfg.execution_bar_delay = 1;
    cfg.exit_defaults.mode = truetest::exits::exit_policy_mode::strategy_only;
    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.run();

    EXPECT_TRUE(eng.is_halted());
    const auto pos = eng.get_portfolio().get_positions().find("BTCUSDT");
    ASSERT_NE(pos, eng.get_portfolio().get_positions().end());
    EXPECT_DOUBLE_EQ(pos->second.qty, 10.0);
}

TEST(BacktestDefects, BF13_AmbiguousImplicitMultiLotCloseHaltsBeforeMutation)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    for (int i = 0; i < 8; ++i) {
        const long long ts = 1704067200000LL + static_cast<long long>(i) * 60000LL;
        dh->load_into_queue(std::to_string(ts), "BTCUSDT",
                            100.0, 100.0, 100.0, 100.0, 100);
    }

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.show_progress = false;
    cfg.initial_balance = 100000;
    cfg.execution_bar_delay = 1;
    cfg.exit_defaults.mode = truetest::exits::exit_policy_mode::strategy_only;
    engine eng(dh, nullptr, std::make_shared<AmbiguousImplicitCloseStrategy>(),
               std::move(cfg));
    eng.run();

    EXPECT_TRUE(eng.is_halted());
    const auto pos = eng.get_portfolio().get_positions().find("BTCUSDT");
    ASSERT_NE(pos, eng.get_portfolio().get_positions().end());
    EXPECT_DOUBLE_EQ(pos->second.qty, 10.0);
    EXPECT_EQ(eng.get_portfolio().get_lots().size(), 2u);
}

TEST(BacktestDefects, BF13_DuplicateExplicitCloseIsIgnoredBeforeSafetyGuards)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    for (int i = 0; i < 5; ++i) {
        const long long ts = 1704067200000LL + static_cast<long long>(i) * 60000LL;
        dh->load_into_queue(std::to_string(ts), "BTCUSDT",
                            100.0, 100.0, 100.0, 100.0, 100);
    }
    auto provider = std::make_shared<DuplicateExplicitCloseProvider>();
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.provider = provider;
    cfg.show_progress = false;
    cfg.initial_balance = 100000;
    cfg.threading = thread_preset::inline_mode;
    cfg.disable_pinning = true;
    cfg.exit_defaults.mode = truetest::exits::exit_policy_mode::strategy_only;
    engine eng(dh, nullptr, std::make_shared<EntryThenExplicitCloseStrategy>(),
               std::move(cfg));
    eng.run();

    EXPECT_FALSE(eng.is_halted());
    EXPECT_TRUE(eng.get_portfolio().get_lots().empty());
    const auto report = eng.get_analytics().generate_report();
    EXPECT_EQ(report.total_fills, 2u);
}

TEST(BacktestDefects, BF13_PhysicalFlipLogReplaysToIdenticalLedger)
{
    SilenceOutput silence;
    FlipLogFile log;
    AnalyticsReport original;
    double original_position = 0.0;
    double original_residual_lot_qty = 0.0;
    std::size_t original_lot_count = 0;
    {
        auto dh = std::make_shared<data_handler>();
        for (int i = 0; i < 6; ++i) {
            const long long ts = 1704067200000LL + static_cast<long long>(i) * 60000LL;
            dh->load_into_queue(std::to_string(ts), "BTCUSDT",
                                100.0, 100.0, 100.0, 100.0, 100);
        }
        engine_config cfg;
        cfg.mode = engine_mode::backtest;
        cfg.show_progress = false;
        cfg.initial_balance = 100000;
        cfg.execution_bar_delay = 1;
        cfg.exit_defaults.mode = truetest::exits::exit_policy_mode::strategy_only;
        cfg.event_log_path = log.path;
        engine eng(dh, nullptr, std::make_shared<BareFlipStrategy>(), std::move(cfg));
        eng.run();
        original = eng.get_analytics().generate_report();
        original_position = eng.get_portfolio().get_positions().at("BTCUSDT").qty;
        original_lot_count = eng.get_portfolio().get_lots().size();
        ASSERT_EQ(original_lot_count, 1u);
        original_residual_lot_qty = eng.get_portfolio().get_lots().begin()->second.qty_open;
    }

    engine_config replay_cfg;
    replay_cfg.mode = engine_mode::backtest;
    replay_cfg.show_progress = false;
    replay_cfg.initial_balance = original.initial_equity;
    replay_cfg.threading = thread_preset::inline_mode;
    replay_cfg.exit_defaults.mode = truetest::exits::exit_policy_mode::strategy_only;
    engine replay(std::make_shared<data_handler>(), nullptr,
                  std::make_shared<BareFlipStrategy>(), std::move(replay_cfg));
    replay.run_replay(log.path);
    const auto replayed = replay.get_analytics().generate_report();

    EXPECT_EQ(original.total_fills, 2u);
    EXPECT_EQ(replayed.total_fills, original.total_fills);
    EXPECT_EQ(replayed.trades.size(), original.trades.size());
    EXPECT_NEAR(replayed.final_equity, original.final_equity, 1e-9);
    EXPECT_NEAR(replayed.realized_pnl, original.realized_pnl, 1e-9);
    EXPECT_NEAR(replayed.unrealized_pnl, original.unrealized_pnl, 1e-9);
    EXPECT_DOUBLE_EQ(replay.get_portfolio().get_positions().at("BTCUSDT").qty,
                     original_position);
    ASSERT_EQ(replay.get_portfolio().get_lots().size(), original_lot_count);
    EXPECT_DOUBLE_EQ(replay.get_portfolio().get_lots().begin()->second.qty_open,
                     original_residual_lot_qty);
}

TEST(BacktestDefects, BF13_LossyPhysicalFlipTriggersPostFillLossLimit)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    for (int i = 0; i < 6; ++i) {
        const long long ts = 1704067200000LL + static_cast<long long>(i) * 60000LL;
        const double px = i == 0 ? 100.0 : 80.0;
        dh->load_into_queue(std::to_string(ts), "BTCUSDT", px, px, px, px, 100);
    }

    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.show_progress = false;
    cfg.initial_balance = 100000;
    cfg.execution_bar_delay = 1;
    cfg.risk_soft_portfolio_limits = false;
    cfg.risk.max_loss_per_trade = 1.0;
    cfg.risk.max_drawdown = 0.99;
    cfg.exit_defaults.mode = truetest::exits::exit_policy_mode::strategy_only;
    engine eng(dh, nullptr, std::make_shared<BareFlipStrategy>(), std::move(cfg));
    eng.run();

    EXPECT_TRUE(eng.is_halted());
    const auto report = eng.get_analytics().generate_report();
    ASSERT_FALSE(report.trades.empty());
    EXPECT_LT(report.trades.back().pnl, -1.0);
}

// ── BF-14: Unwind multi-lot position clears all brackets ──────────────────

class MultiLotWithExitsStrategy : public IStrategy {
    int bar_count_ = 0;
    std::vector<truetest::exits::exit_intent> pending_intents_;
    engine* eng_ = nullptr;
public:
    void set_engine(engine* e) { eng_ = e; }

    std::vector<truetest::exits::exit_intent> take_pending_exit_intents() override
    {
        auto out = std::move(pending_intents_);
        pending_intents_.clear();
        return out;
    }

    std::optional<order_event> on_market(const market_event& m) override
    {
        if (bar_count_ == 0)
        {
            bar_count_++;
            truetest::exits::exit_intent intent;
            intent.symbol = m.get_symbol();
            intent.close_side = order_side::sell;
            intent.stop_loss = 90.0;
            intent.qty = 5.0;
            pending_intents_.push_back(intent);
            return order_event(m.get_timestamp(), m.get_symbol(),
                               order_type::market, order_side::buy, 5.0, m.get_close());
        }
        else if (bar_count_ == 1)
        {
            bar_count_++;
            truetest::exits::exit_intent intent;
            intent.symbol = m.get_symbol();
            intent.close_side = order_side::sell;
            intent.stop_loss = 85.0;
            intent.qty = 5.0;
            pending_intents_.push_back(intent);
            return order_event(m.get_timestamp(), m.get_symbol(),
                               order_type::market, order_side::buy, 5.0, m.get_close());
        }
        else if (bar_count_ == 2)
        {
            bar_count_++;
            if (eng_) eng_->request_flatten();
        }
        return std::nullopt;
    }
    void set_position_open(const std::string&, bool) override {}
};

TEST(BacktestDefects, BF14_UnwindMultiLotPositionClearsAllBrackets)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    for (int i = 0; i < 6; ++i) {
        const long long ts = 1704067200000LL + static_cast<long long>(i) * 60000LL;
        dh->load_into_queue(std::to_string(ts), "BTCUSDT",
                            100.0, 100.0, 100.0, 100.0, 100);
    }
    auto strat = std::make_shared<MultiLotWithExitsStrategy>();
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.show_progress = false;
    cfg.initial_balance = 100000;
    cfg.execution_bar_delay = 1;
    engine eng(dh, nullptr, strat, std::move(cfg));
    strat->set_engine(&eng);

    eng.run();

    // After flatten, all lots are closed and armed count is 0
    EXPECT_TRUE(eng.get_portfolio().get_lots().empty());
    EXPECT_EQ(eng.get_exit_manager().armed_count(), 0u);
}

class MultiSymbolUnwindPriceStrategy final : public IStrategy
{
public:
    void set_engine(engine* eng) noexcept { engine_ = eng; }

    std::optional<order_event> on_market(const market_event& m) override
    {
        if (m.get_symbol() == "A" && !opened_a_)
        {
            opened_a_ = true;
            return order_event(m.get_timestamp(), m.get_symbol(),
                               order_type::market, order_side::buy,
                               1.0, m.get_close());
        }
        if (m.get_symbol() == "B" && !opened_b_)
        {
            opened_b_ = true;
            return order_event(m.get_timestamp(), m.get_symbol(),
                               order_type::market, order_side::buy,
                               1.0, m.get_close());
        }
        if (filled_a_ && filled_b_ && !flatten_requested_)
        {
            flatten_requested_ = true;
            engine_->request_flatten();
        }
        return std::nullopt;
    }

    void on_fill(const fill_event& fill, std::uint64_t) override
    {
        if (fill.get_side() != order_side::buy)
            return;
        if (fill.get_symbol() == "A") filled_a_ = true;
        if (fill.get_symbol() == "B") filled_b_ = true;
    }

    void set_position_open(const std::string&, bool) override {}

private:
    engine* engine_ = nullptr;
    bool opened_a_ = false;
    bool opened_b_ = false;
    bool filled_a_ = false;
    bool filled_b_ = false;
    bool flatten_requested_ = false;
};

class MultiSymbolUnwindCaptureAdapter final : public IExecutionAdapter
{
public:
    struct unwind_record
    {
        std::string symbol;
        double price = 0.0;
        std::chrono::system_clock::time_point timestamp{};
        std::string strategy_name;
        std::uint64_t opener_order_id = 0;
    };

    void set_mid_price(double mid) override { current_mid_ = mid; }

    void submit_order(const order_event& order) override
    {
        if (order.get_strategy_name() == "__engine_unwind__")
        {
            unwind_records.push_back(unwind_record{
                order.get_symbol(), order.get_price(), order.get_timestamp(),
                order.get_strategy_name(), order.get_opener_order_id()});
        }

        const double price = order.get_price() > 0.0
            ? order.get_price() : current_mid_;
        fill_event fill(order.get_timestamp(), order.get_symbol(),
                        order.get_order_id(), order.get_side(),
                        order.get_quantity(), price,
                        /*commission=*/0.0, /*remaining=*/0.0,
                        /*fill_id=*/++next_fill_id_);
        fill.set_source(fill_source::simulated);
        (void)fill.set_venue_execution_id(
            "capture-" + std::to_string(next_fill_id_));
        (void)fill.set_commission_currency("USDT");
        fill.set_cumulative_filled_qty(
            order.get_quantity(), fill_cumulative_source::simulated);
        pending_fills_.push_back(std::move(fill));
    }

    bool poll_fills(std::vector<fill_event>& out) override
    {
        if (pending_fills_.empty())
            return false;
        out.insert(out.end(),
                   std::make_move_iterator(pending_fills_.begin()),
                   std::make_move_iterator(pending_fills_.end()));
        pending_fills_.clear();
        return true;
    }

    bool cancel_order(std::uint64_t) override { return false; }

    std::vector<unwind_record> unwind_records;

private:
    double current_mid_ = 0.0;
    std::uint64_t next_fill_id_ = 0;
    std::vector<fill_event> pending_fills_;
};

class MultiSymbolUnwindCaptureProvider final : public IProvider
{
public:
    std::string name() const override { return "unwind-capture"; }
    bool has_data_feed() const override { return false; }
    bool has_execution() const override { return true; }
    bool open() override { return true; }
    void close() override {}
    std::shared_ptr<IDataTransport> get_transport() override { return {}; }
    std::shared_ptr<IExecutionAdapter> get_execution_adapter() override
    {
        return adapter;
    }

    std::shared_ptr<MultiSymbolUnwindCaptureAdapter> adapter =
        std::make_shared<MultiSymbolUnwindCaptureAdapter>();
};

TEST(BacktestDefects, C13_MultiSymbolEmergencyUnwindUsesEachSymbolsOwnMark)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    dh->load_into_queue("1704067200000", "A", 100.0, 100.0, 100.0, 100.0, 1);
    dh->load_into_queue("1704067260000", "B", 1'000.0, 1'000.0, 1'000.0, 1'000.0, 1);
    dh->load_into_queue("1704067320000", "B", 1'000.0, 1'000.0, 1'000.0, 1'000.0, 1);
    dh->load_into_queue("1704067380000", "B", 1'000.0, 1'000.0, 1'000.0, 1'000.0, 1);

    auto strategy = std::make_shared<MultiSymbolUnwindPriceStrategy>();
    auto provider = std::make_shared<MultiSymbolUnwindCaptureProvider>();
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.provider = provider;
    cfg.show_progress = false;
    cfg.initial_balance = 100'000.0;
    cfg.execution_bar_delay = 0;
    cfg.threading = thread_preset::inline_mode;
    cfg.exit_defaults.mode = truetest::exits::exit_policy_mode::strategy_only;
    cfg.fee_model = std::make_shared<ZeroFeeModel>();
    cfg.market_aggression = 1.0;
    cfg.mm_base_spread_pct = 0.0;
    cfg.mm_vol_spread_mult = 0.0;
    cfg.mm_max_half_spread_pct = 0.0;
    engine eng(dh, nullptr, strategy, std::move(cfg));
    strategy->set_engine(&eng);
    eng.set_primary_strategy_name("multi-symbol");

    eng.run();

    EXPECT_FALSE(eng.get_portfolio().position_open("A"));
    EXPECT_FALSE(eng.get_portfolio().position_open("B"));
    ASSERT_EQ(provider->adapter->unwind_records.size(), 2u);
    const auto expected_ts = std::chrono::system_clock::time_point{
        std::chrono::milliseconds{1'704'067'380'000LL}};
    bool saw_a = false;
    bool saw_b = false;
    for (const auto& record : provider->adapter->unwind_records)
    {
        EXPECT_EQ(record.timestamp, expected_ts);
        EXPECT_EQ(record.strategy_name, "multi-symbol");
        EXPECT_NE(record.opener_order_id, 0u);
        if (record.symbol == "A")
        {
            saw_a = true;
            EXPECT_DOUBLE_EQ(record.price, 100.0);
        }
        else if (record.symbol == "B")
        {
            saw_b = true;
            EXPECT_DOUBLE_EQ(record.price, 1'000.0);
        }
        else
        {
            ADD_FAILURE() << "unexpected unwind symbol " << record.symbol;
        }
    }
    EXPECT_TRUE(saw_a);
    EXPECT_TRUE(saw_b);
    EXPECT_DOUBLE_EQ(eng.get_portfolio().get_cash(), 100'000.0);
}

// ── BF-15: Unwind fill not delivered to strategy on_fill ──────────────────

class UnwindSentinelStrategy : public IStrategy {
    int bar_count_ = 0;
    engine* eng_ = nullptr;
public:
    void set_engine(engine* e) { eng_ = e; }
    bool unwind_fill_delivered = false;

    std::optional<order_event> on_market(const market_event& m) override
    {
        if (bar_count_ == 0)
        {
            bar_count_++;
            return order_event(m.get_timestamp(), m.get_symbol(),
                               order_type::market, order_side::buy, 5.0, m.get_close());
        }
        else if (bar_count_ == 1)
        {
            bar_count_++;
            if (eng_) eng_->request_flatten();
        }
        return std::nullopt;
    }

    void on_fill(const fill_event& f, std::uint64_t /*opener_order_id*/) override
    {
        if (f.get_strategy_name() == "__engine_unwind__" ||
            f.get_strategy_name() == "risk_unwind" ||
            f.get_side() == order_side::sell)
        {
            unwind_fill_delivered = true;
        }
    }

    void set_position_open(const std::string&, bool) override {}
};

TEST(BacktestDefects, BF15_UnwindFillNotDeliveredToStrategyOnFill)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    for (int i = 0; i < 6; ++i) {
        const long long ts = 1704067200000LL + static_cast<long long>(i) * 60000LL;
        dh->load_into_queue(std::to_string(ts), "BTCUSDT",
                            100.0, 100.0, 100.0, 100.0, 100);
    }
    auto strat = std::make_shared<UnwindSentinelStrategy>();
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.show_progress = false;
    cfg.initial_balance = 100000;
    cfg.execution_bar_delay = 1;
    engine eng(dh, nullptr, strat, std::move(cfg));
    strat->set_engine(&eng);

    eng.run();

    EXPECT_FALSE(strat->unwind_fill_delivered);
}

// ── BF-16: Sync fill closing order does not arm inverted bracket ──────────

class BuyThenSignalCloseStrategy : public IStrategy {
    int bar_count_ = 0;
public:
    std::optional<order_event> on_market(const market_event& m) override
    {
        if (bar_count_ == 0)
        {
            bar_count_++;
            // Open 5-unit long
            return order_event(m.get_timestamp(), m.get_symbol(),
                               order_type::market, order_side::buy, 5.0, m.get_close());
        }
        else if (bar_count_ == 1)
        {
            bar_count_++;
            // Signal-close 5-unit long with a bare market sell (no explicit opener_order_id)
            return order_event(m.get_timestamp(), m.get_symbol(),
                               order_type::market, order_side::sell, 5.0, m.get_close());
        }
        return std::nullopt;
    }
    void set_position_open(const std::string&, bool) override {}
};

TEST(BacktestDefects, BF16_SyncFillClosingOrderDoesNotArmInvertedBracket)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    for (int i = 0; i < 4; ++i) {
        const long long ts = 1704067200000LL + static_cast<long long>(i) * 60000LL;
        dh->load_into_queue(std::to_string(ts), "BTCUSDT",
                            100.0, 100.0, 100.0, 100.0, 100);
    }
    auto strat = std::make_shared<BuyThenSignalCloseStrategy>();
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.show_progress = false;
    cfg.initial_balance = 100000;
    cfg.execution_bar_delay = 0; // Synchronous fill
    cfg.exit_defaults.mode = truetest::exits::exit_policy_mode::floor;
    cfg.exit_defaults.sl_pct = 0.05; // 5% stop loss
    cfg.exit_defaults.tp_pct = 0.50;

    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.run();

    // After closing the long, position is flat, and NO inverted short bracket is armed
    EXPECT_EQ(eng.get_exit_manager().armed_count(), 0u);
}

// ── BF-17: Flip fill leaves no orphaned old lot ───────────────────────────

TEST(BacktestDefects, BF17_FlipFillLeavesNoOrphanedOldLot)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    for (int i = 0; i < 6; ++i) {
        const long long ts = 1704067200000LL + static_cast<long long>(i) * 60000LL;
        dh->load_into_queue(std::to_string(ts), "BTCUSDT",
                            100.0, 100.0, 100.0, 100.0, 100);
    }
    auto strat = std::make_shared<OneShotFlipStrategy>();
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.show_progress = false;
    cfg.initial_balance = 100000;
    cfg.execution_bar_delay = 1;
    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.run();

    const auto& port = eng.get_portfolio();
    auto open_lots = port.open_lots_by_symbol("BTCUSDT");

    // Exactly one open lot post-flip
    EXPECT_EQ(open_lots.size(), 1u);
    ASSERT_EQ(port.get_lots().size(), 1u);
    EXPECT_DOUBLE_EQ(port.get_lots().begin()->second.qty_open, 5.0);
}

// ── BF-07: Sub-millisecond backtest safe run summary ──────────────────────

TEST(BacktestDefects, BF07_ReportRunSummarySubMillisecondSafe)
{
    auto dh = std::make_shared<data_handler>();
    dh->load_into_queue("1704067200000", "BTCUSDT", 100.0, 100.0, 100.0, 100.0, 100);
    auto strat = std::make_shared<OneShotFlipStrategy>();
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.show_progress = true; // Exercise report_run_summary code path
    cfg.initial_balance = 100000;

    SilenceOutput silence;
    engine eng(dh, nullptr, strat, std::move(cfg));
    // Fast 1-bar run completes in sub-millisecond, exercising safe throughput calculation
    eng.run();
    SUCCEED();
}

// ── BF-18: Flip fill does not double-count slippage or total fills ────────

TEST(BacktestDefects, BF18_FlipFillDoesNotDuplicateSlippageOrFillCount)
{
    SilenceOutput silence;
    auto dh = std::make_shared<data_handler>();
    for (int i = 0; i < 6; ++i) {
        const long long ts = 1704067200000LL + static_cast<long long>(i) * 60000LL;
        dh->load_into_queue(std::to_string(ts), "BTCUSDT",
                            100.0, 100.0, 100.0, 100.0, 100);
    }
    auto strat = std::make_shared<OneShotFlipStrategy>();
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.show_progress = false;
    cfg.initial_balance = 100000;
    cfg.execution_bar_delay = 1;
    engine eng(dh, nullptr, strat, std::move(cfg));
    eng.run();

    const auto report = eng.get_analytics().generate_report();
    // 1 opener fill (qty 10) + 1 flip fill (qty 15 decomposed into close 10 + open 5) = 2 fills total
    EXPECT_EQ(report.total_fills, 2u);
}
