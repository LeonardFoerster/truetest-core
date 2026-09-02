// Data load / parse / streaming timestamp locks (backtest defect closure).
#include "helpers/backtest_defect_helpers.h"

TEST(BacktestDefects, DR01_GbmTimestampsStrictlyIncreasing)
{
    GBMGenerator gen;
    auto cfg = gen.default_config();
    cfg.n_steps = 100;
    auto path = gen.generate(42, cfg);
    ASSERT_EQ(path.bars.size(), 100u);

    std::chrono::system_clock::time_point prev{};
    for (size_t i = 0; i < path.bars.size(); ++i) {
        auto tp = tt::date_parse::parse(path.bars[i].date);
        ASSERT_TRUE(tp.has_value()) << "bar " << i << " date=" << path.bars[i].date;
        if (i > 0) {
            auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(*tp - prev).count();
            EXPECT_GE(delta, 1000) << "bar " << i << " spacing too small";
            EXPECT_EQ(delta, 60000) << "expected 1-minute synthetic bars";
        }
        prev = *tp;
    }
}

TEST(BacktestDefects, DR01_DateParse_HHMM_NoMidnightCollapse)
{
    auto r = tt::date_parse::parse("2024-01-01 12:34");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->time_since_epoch(),
              std::chrono::seconds(1704067200 + 12 * 3600 + 34 * 60));
}

// ── DR-03/04: sort bars/ticks ──────────────────────────────────────────────

TEST(BacktestDefects, DR04_SortBarsByTime)
{
    data_handler dh;
    dh.load_into_queue("1704067320000", "A", 102, 103, 101, 102, 1); // later
    dh.load_into_queue("1704067200000", "A", 100, 101, 99, 100, 1);  // earlier
    dh.sort_bars_by_time();
    EXPECT_EQ(dh.bar_at(0).date, "1704067200000");
    EXPECT_EQ(dh.bar_at(1).date, "1704067320000");
}

TEST(BacktestDefects, DR03_OutOfOrderTicksAcceptedThenSorted)
{
    data_handler dh;
    tick_record a, b;
    a.timestamp = t_at(200);
    a.symbol = "X";
    a.price = 2.0;
    a.quantity = 1;
    b.timestamp = t_at(100);
    b.symbol = "X";
    b.price = 1.0;
    b.quantity = 1;
    EXPECT_TRUE(dh.add_tick(a));
    EXPECT_TRUE(dh.add_tick(b)); // previously skipped
    EXPECT_EQ(dh.tick_count(), 2u);
    dh.sort_ticks_by_time();
    EXPECT_EQ(dh.tick_at(0).timestamp, t_at(100));
    EXPECT_EQ(dh.tick_at(1).timestamp, t_at(200));
}

// ── FR-06: multi-symbol equity ─────────────────────────────────────────────

TEST(BacktestDefects, DR02_InvalidBarsCounted)
{
    data_handler dh;
    EXPECT_FALSE(dh.load_into_queue("1704067200000", "A", -1, 1, 1, 1, 1));
    EXPECT_FALSE(dh.load_into_queue("1704067200000", "A", 1, 1, 2, 1, 1)); // h < l
    EXPECT_TRUE(dh.load_into_queue("1704067200000", "A", 1, 2, 0.5, 1, 1));
    EXPECT_EQ(dh.validation_errors(), 2u);
    EXPECT_EQ(dh.bar_count(), 1u);
}

// ── DR-REPLAY-02: DataBridge parse rejects accounted in LoadStats ──────────

TEST(BacktestDefects, DR_REPLAY_02_DataBridgeParseRejectsInLoadStats)
{
    SilenceOutput silence;
    // Mix of: (1) parse_record nullopt (non-numeric OHLC) and (2) empty line.
    // Both used to vanish without LoadStats.rejected; validation_errors() only
    // sees sink rejects, so CLI must stamp data_rows_rejected from LoadStats.
    auto transport = std::make_shared<MockBatchTransport>(std::vector<std::string>{
        "date,symbol,open,high,low,close,volume",
        "2024-01-01,AAPL,150.0,155.0,149.0,153.0,1000000",
        "this,is,not,a,valid,ohlc,row",
        "",
        "2024-01-02,AAPL,153.0,158.0,152.0,157.0,1200000",
        "xxxx,SYM,nope,nope,nope,nope,0",
    });
    auto parser = std::make_shared<CsvBarParser>();
    auto bridge = std::make_shared<DataBridge<bar_record>>(
        transport, parser, bar_record_sink);

    data_handler dh;
    LoadStats stats;
    ASSERT_TRUE(bridge->load_into(dh, &stats));
    EXPECT_EQ(stats.accepted, 2u);
    EXPECT_EQ(stats.rejected, 3u) << "malformed/empty frames must not be silent drops";
    EXPECT_EQ(dh.bar_count(), 2u);
    // Pure parse failures never reach the sink — validation_errors undercounts
    // total rejects. LoadStats.rejected is the complete counter for CLI.
    EXPECT_EQ(dh.validation_errors(), 0u);
    EXPECT_GT(stats.rejected, dh.validation_errors());

    Analytics a(10000.0);
    a.set_data_rows_rejected(stats.rejected);
    EXPECT_EQ(a.snapshot().data_rows_rejected, 3u);
}

// ── FR-01 router path: engine with maker_queue_model still fills markets ──

TEST(BacktestDefects, DataWrapper_SortAfterLoadOrdersTicks)
{
    // Tiny source that emits out-of-order ticks — exercises shipped DataWrapper::load.
    class OooTickSource : public IMarketSource {
    public:
        bool load_into(IMarketSink& sink, LoadStats* stats) override
        {
            tick_record late, early;
            late.timestamp = t_at(500);
            late.symbol = "X";
            late.price = 2.0;
            late.quantity = 1;
            early.timestamp = t_at(100);
            early.symbol = "X";
            early.price = 1.0;
            early.quantity = 1;
            // Intentionally late then early.
            if (!sink.on_tick(late)) return false;
            if (!sink.on_tick(early)) return false;
            if (stats) stats->accepted = 2;
            return true;
        }
    };

    DataLoadOptions opt;
    opt.sort_after_load = true;
    opt.fail_if_empty = true;
    auto w = DataWrapper::from_source(std::make_unique<OooTickSource>(), opt);
    MarketSeries out;
    ASSERT_TRUE(w.load(out));
    ASSERT_EQ(out.tick_count(), 2u);
    EXPECT_EQ(out.tick_at(0).timestamp, t_at(100));
    EXPECT_EQ(out.tick_at(1).timestamp, t_at(500));
}

TEST(BacktestDefects, DR_DataWrapperSortAllKeepsRowsAndTicksAligned)
{
    class ThreeRowsAndTicks final : public IMarketSource {
    public:
        bool load_into(IMarketSink& sink, LoadStats* stats) override
        {
            // Sorted sources form a 3-cycle, a separate 2-cycle, and a
            // fixed point: [2, 0, 1, 4, 3, 5].
            for (const auto ms : {200, 300, 100, 500, 400, 600})
            {
                Bar bar;
                bar.ts = t_at(ms);
                bar.date = std::to_string(ms);
                bar.symbol = "BAR" + std::to_string(ms);
                bar.open = static_cast<double>(ms) + 0.1;
                bar.high = static_cast<double>(ms) + 0.5;
                bar.low = static_cast<double>(ms) - 0.3;
                bar.close = static_cast<double>(ms) + 0.4;
                bar.volume = ms * 10;
                EXPECT_TRUE(sink.on_bar(bar));

                Tick tick;
                tick.timestamp = t_at(ms);
                tick.symbol = "TICK" + std::to_string(ms);
                tick.price = static_cast<double>(ms);
                tick.quantity = ms * 100;
                tick.side = (ms == 300 || ms == 400)
                    ? data_tick_side::ask : data_tick_side::bid;
                EXPECT_TRUE(sink.on_tick(tick));
            }
            if (stats) stats->accepted = 12;
            return true;
        }
    };

    SilenceOutput silence;
    auto wrapper = DataWrapper::from_source(std::make_unique<ThreeRowsAndTicks>());
    MarketSeries series;
    ASSERT_TRUE(wrapper.load(series));

    ASSERT_EQ(series.bar_count(), 6u);
    ASSERT_EQ(series.tick_count(), 6u);
    for (std::size_t i = 0; i < 6; ++i)
    {
        const int ms = static_cast<int>((i + 1) * 100);
        const auto bar = series.bar_at(i);
        EXPECT_EQ(bar.ts, t_at(ms));
        EXPECT_EQ(bar.date, std::to_string(ms));
        EXPECT_EQ(bar.symbol, "BAR" + std::to_string(ms));
        EXPECT_DOUBLE_EQ(bar.open, static_cast<double>(ms) + 0.1);
        EXPECT_DOUBLE_EQ(bar.high, static_cast<double>(ms) + 0.5);
        EXPECT_DOUBLE_EQ(bar.low, static_cast<double>(ms) - 0.3);
        EXPECT_DOUBLE_EQ(bar.close, static_cast<double>(ms) + 0.4);
        EXPECT_EQ(bar.volume, ms * 10);

        const auto& tick = series.tick_at(i);
        EXPECT_EQ(tick.timestamp, t_at(ms));
        EXPECT_EQ(tick.symbol, "TICK" + std::to_string(ms));
        EXPECT_DOUBLE_EQ(tick.price, static_cast<double>(ms));
        EXPECT_EQ(tick.quantity, ms * 100);
        EXPECT_EQ(tick.side, (ms == 300 || ms == 400)
            ? data_tick_side::ask : data_tick_side::bid);
    }
}

// ── EL-MULTISYM-MID: delayed fill mid tracks order symbol marks ────────────

TEST(BacktestDefects, DR_StreamingBarTs_FromOpenTimeNotWallClock)
{
    SilenceOutput silence;
    auto transport = std::make_shared<MockStreamingTransport>();
    auto parser = std::make_shared<CsvBarParser>();
    auto bridge = std::make_shared<DataBridge<bar_record>>(
        transport, parser, bar_record_sink);

    class CaptureTs : public IStrategy {
    public:
        std::vector<std::chrono::system_clock::time_point> ts;
        std::optional<order_event> on_market(const market_event& m) override
        {
            ts.push_back(m.get_timestamp());
            return std::nullopt;
        }
        void set_position_open(const std::string&, bool) override {}
    };

    auto dh = std::make_shared<data_handler>();
    auto strat = std::make_shared<CaptureTs>();
    engine_config cfg;
    cfg.mode = engine_mode::backtest;
    cfg.seed = 42;
    cfg.show_progress = false;
    cfg.initial_balance = 10000;
    cfg.exit_defaults.mode = truetest::exits::exit_policy_mode::strategy_only;
    engine eng(dh, nullptr, strat, std::move(cfg));

    // ISO date (not epoch-in-date) + open_time epoch-ms — old path used wall clock.
    const long long ot0 = 1577836800000LL; // 2020-01-01 00:00:00 UTC
    std::thread feeder([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        transport->enqueue("date,open_time,symbol,open,high,low,close,volume");
        for (int i = 0; i < 4; ++i) {
            const long long ot = ot0 + static_cast<long long>(i) * 60000LL;
            transport->enqueue(
                "2020-01-01," + std::to_string(ot) + ",BTCUSDT,100,101,99,100,1000");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        transport->request_stop();
    });

    eng.run_streaming(bridge);
    feeder.join();

    ASSERT_EQ(strat->ts.size(), 4u);
    for (int i = 0; i < 4; ++i) {
        const auto expected = std::chrono::system_clock::time_point{
            std::chrono::milliseconds{ot0 + static_cast<long long>(i) * 60000LL}};
        EXPECT_EQ(strat->ts[static_cast<std::size_t>(i)], expected)
            << "bar " << i << " must use open_time_ms, not wall clock";
    }
}

TEST(BacktestDefects, DR_MultiSource_FailClosedOnPartFailure)
{
    // DR-REPLAY-03: default soft-success removed.
    // Create two paths; second does not exist → load fails entirely.
    DataLoadOptions opt;
    opt.fail_if_empty = true;
    opt.allow_partial_sources = false;
    auto w = DataWrapper::from_paths(
        {std::filesystem::path("/tmp/truetest_missing_a.csv"),
         std::filesystem::path("/tmp/truetest_missing_b.csv")},
        opt);
    MarketSeries series;
    EXPECT_FALSE(w.load(series));
    EXPECT_TRUE(series.empty());
}

TEST(BacktestDefects, DR_MultiSource_FailClosedRollsBackEarlierRows)
{
    SilenceOutput silence;
    MarketSeries series;
    ASSERT_TRUE(series.load_into_queue(
        "2024-01-01", "PREEXISTING", 10.0, 11.0, 9.0, 10.0, 1));

    const auto fixture = std::filesystem::path(TEST_FIXTURES_DIR) / "sample_ohlcv.csv";
    const auto missing = std::filesystem::path(TEST_FIXTURES_DIR)
        / "missing_datawrapper_transaction_fixture.csv";

    auto wrapper = DataWrapper::from_paths({fixture, missing});
    EXPECT_FALSE(wrapper.load(series));

    ASSERT_EQ(series.bar_count(), 1u);
    EXPECT_EQ(series.bar_symbol_at(0), "PREEXISTING");
    EXPECT_EQ(series.validation_errors(), 0u);
}

TEST(BacktestDefects, DR_MultiSource_PartialModeContinuesAfterFailure)
{
    SilenceOutput silence;
    const auto fixture = std::filesystem::path(TEST_FIXTURES_DIR) / "sample_ohlcv.csv";
    const auto missing = std::filesystem::path(TEST_FIXTURES_DIR)
        / "missing_datawrapper_partial_fixture.csv";

    DataLoadOptions opt;
    opt.allow_partial_sources = true;
    auto wrapper = DataWrapper::from_paths({fixture, missing, fixture}, opt);
    MarketSeries series;

    ASSERT_TRUE(wrapper.load(series));
    ASSERT_EQ(series.bar_count(), 4u);
    EXPECT_EQ(series.bar_symbol_at(0), "AAPL");
    EXPECT_EQ(series.bar_symbol_at(2), "AAPL");
}

TEST(BacktestDefects, DR_MultiSourceTickFailurePreservesPriorOrder)
{
    SilenceOutput silence;
    MarketSeries series;
    tick_record late;
    late.timestamp = t_at(200);
    late.symbol = "PREEXISTING";
    late.price = 2.0;
    late.quantity = 1;
    tick_record early = late;
    early.timestamp = t_at(100);
    early.price = 1.0;
    ASSERT_TRUE(series.add_tick(late));
    ASSERT_TRUE(series.add_tick(early));

    const auto fixture = std::filesystem::path(TEST_FIXTURES_DIR) / "sample_ticks.csv";
    const auto missing = std::filesystem::path(TEST_FIXTURES_DIR)
        / "missing_datawrapper_transaction_ticks.csv";
    auto wrapper = DataWrapper::from_paths({fixture, missing});

    EXPECT_FALSE(wrapper.load(series));
    ASSERT_EQ(series.tick_count(), 2u);
    EXPECT_EQ(series.tick_at(0).timestamp, t_at(200));
    EXPECT_EQ(series.tick_at(1).timestamp, t_at(100));
}

TEST(BacktestDefects, DR_DataWrapperRollbackSurvivesThrowAfterAppend)
{
    class AppendsThenThrows final : public IMarketSource {
    public:
        bool load_into(IMarketSink& sink, LoadStats*) override
        {
            Bar good;
            good.date = "2024-01-02";
            good.symbol = "NEW";
            good.open = good.high = good.low = good.close = 20.0;
            good.volume = 1;
            EXPECT_TRUE(sink.on_bar(good));

            Bar invalid = good;
            invalid.open = 0.0;
            EXPECT_FALSE(sink.on_bar(invalid));
            throw std::runtime_error("synthetic source failure");
        }
    };

    SilenceOutput silence;
    MarketSeries series;
    ASSERT_TRUE(series.load_into_queue(
        "2024-01-01", "PREEXISTING", 10.0, 11.0, 9.0, 10.0, 1));
    auto wrapper = DataWrapper::from_source(std::make_unique<AppendsThenThrows>());

    EXPECT_THROW(wrapper.load(series), std::runtime_error);
    ASSERT_EQ(series.bar_count(), 1u);
    EXPECT_EQ(series.bar_symbol_at(0), "PREEXISTING");
    EXPECT_EQ(series.validation_errors(), 0u);
}

TEST(BacktestDefects, DR_DataWrapperFalseAfterAppendRollsBack)
{
    class AppendsThenFails final : public IMarketSource {
    public:
        bool load_into(IMarketSink& sink, LoadStats* stats) override
        {
            Bar bar;
            bar.date = "2024-01-02";
            bar.symbol = "NEW";
            bar.open = bar.high = bar.low = bar.close = 20.0;
            bar.volume = 1;
            EXPECT_TRUE(sink.on_bar(bar));
            if (stats) stats->accepted = 1;
            return false;
        }
    };

    SilenceOutput silence;
    MarketSeries series;
    ASSERT_TRUE(series.load_into_queue(
        "2024-01-01", "PREEXISTING", 10.0, 11.0, 9.0, 10.0, 1));
    DataLoadOptions opt;
    opt.allow_partial_sources = true;
    auto wrapper = DataWrapper::from_source(std::make_unique<AppendsThenFails>(), opt);

    EXPECT_FALSE(wrapper.load(series));
    ASSERT_EQ(series.bar_count(), 1u);
    EXPECT_EQ(series.bar_symbol_at(0), "PREEXISTING");
}

TEST(BacktestDefects, DR_DataWrapperRejectsShrinkingSource)
{
    class ClearsSeries final : public IMarketSource {
    public:
        bool load_into(IMarketSink& sink, LoadStats*) override
        {
            auto* series = dynamic_cast<MarketSeries*>(&sink);
            EXPECT_NE(series, nullptr);
            if (!series) return false;
            series->clear();
            return true;
        }
    };

    SilenceOutput silence;
    MarketSeries series;
    ASSERT_TRUE(series.load_into_queue(
        "2024-01-01", "PREEXISTING", 10.0, 11.0, 9.0, 10.0, 1));
    auto wrapper = DataWrapper::from_source(std::make_unique<ClearsSeries>());

    EXPECT_THROW(wrapper.load(series), std::logic_error);
}

TEST(BacktestDefects, DR_DataLoadOptions_FromToSymbolsApplied)
{
    // DR-REPLAY-04: from/to/symbols must filter after load.
    class TwoBarSource final : public IMarketSource {
    public:
        bool load_into(IMarketSink& sink, LoadStats* stats) override
        {
            Bar a;
            a.date = "2024-01-01";
            a.symbol = "AAA";
            a.ts = std::chrono::system_clock::time_point(std::chrono::milliseconds(1000));
            a.open = a.high = a.low = a.close = 10.0;
            a.volume = 1;
            Bar b = a;
            b.date = "2024-01-02";
            b.symbol = "BBB";
            b.ts = std::chrono::system_clock::time_point(std::chrono::milliseconds(2000));
            sink.on_bar(a);
            sink.on_bar(b);
            if (stats) { stats->accepted = 2; }
            return true;
        }
    };

    DataLoadOptions opt;
    opt.symbols = {"AAA"};
    opt.sort_after_load = true;
    auto w = DataWrapper::from_source(std::make_unique<TwoBarSource>(), opt);
    MarketSeries series;
    ASSERT_TRUE(w.load(series));
    EXPECT_EQ(series.bar_count(), 1u);
    EXPECT_EQ(series.bar_symbol_at(0), "AAA");
}

TEST(BacktestDefects, DR_DataWrapperFilterPreservesPreexistingRows)
{
    class AddsKeepAndDrop final : public IMarketSource {
    public:
        bool load_into(IMarketSink& sink, LoadStats* stats) override
        {
            Bar keep;
            keep.date = "2024-01-02";
            keep.symbol = "KEEP";
            keep.open = keep.high = keep.low = keep.close = 20.0;
            keep.volume = 1;
            Bar drop = keep;
            drop.symbol = "DROP";
            EXPECT_TRUE(sink.on_bar(keep));
            EXPECT_TRUE(sink.on_bar(drop));
            if (stats) stats->accepted = 2;
            return true;
        }
    };

    SilenceOutput silence;
    MarketSeries series;
    ASSERT_TRUE(series.load_into_queue(
        "2024-01-01", "PREEXISTING", 10.0, 11.0, 9.0, 10.0, 1));
    DataLoadOptions opt;
    opt.symbols = {"KEEP"};
    auto wrapper = DataWrapper::from_source(std::make_unique<AddsKeepAndDrop>(), opt);

    ASSERT_TRUE(wrapper.load(series));
    ASSERT_EQ(series.bar_count(), 2u);
    EXPECT_EQ(series.bar_symbol_at(0), "PREEXISTING");
    EXPECT_EQ(series.bar_symbol_at(1), "KEEP");
}

TEST(BacktestDefects, DR_DataWrapperFilterFailurePreservesPreexistingRows)
{
    class AddsOnlyDrop final : public IMarketSource {
    public:
        bool load_into(IMarketSink& sink, LoadStats* stats) override
        {
            Bar drop;
            drop.date = "2024-01-02";
            drop.symbol = "DROP";
            drop.open = drop.high = drop.low = drop.close = 20.0;
            drop.volume = 1;
            EXPECT_TRUE(sink.on_bar(drop));
            if (stats) stats->accepted = 1;
            return true;
        }
    };

    SilenceOutput silence;
    MarketSeries series;
    ASSERT_TRUE(series.load_into_queue(
        "2024-01-01", "PREEXISTING", 10.0, 11.0, 9.0, 10.0, 1));
    DataLoadOptions opt;
    opt.symbols = {"KEEP"};
    opt.fail_if_empty = true;
    auto wrapper = DataWrapper::from_source(std::make_unique<AddsOnlyDrop>(), opt);

    EXPECT_FALSE(wrapper.load(series));
    ASSERT_EQ(series.bar_count(), 1u);
    EXPECT_EQ(series.bar_symbol_at(0), "PREEXISTING");
}
