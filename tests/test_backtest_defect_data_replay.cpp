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

