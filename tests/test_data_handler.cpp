#include <gtest/gtest.h>
#include "data/data_handler.h"
#include "data/market_series.h"
#include "data/market_types.h"
#include "data/csv_data_source.h"
#include "data/tick_csv_data_source.h"
#include "data/data_wrapper.h"
#include <filesystem>
#include <sstream>

namespace {
// Same-named struct with a different layout exists in other test TUs;
// anonymous-namespace scope prevents LTO ODR-merging from picking the
// wrong definition.
struct SilenceOutput {
    std::ostringstream sink;
    std::streambuf* orig_out;
    std::streambuf* orig_err;
    SilenceOutput()
        : orig_out(std::cout.rdbuf(sink.rdbuf()))
        , orig_err(std::cerr.rdbuf(sink.rdbuf())) {}
    ~SilenceOutput() {
        std::cout.rdbuf(orig_out);
        std::cerr.rdbuf(orig_err);
    }
};
}

// Helper: path to test fixtures relative to the test binary
static std::string fixture_path(const std::string& name)
{
    std::string p = std::string(TEST_FIXTURES_DIR) + "/" + name;
    return p;
}

TEST(DataHandler, LoadIntoQueue)
{
    data_handler dh;
    dh.load_into_queue("2024-01-01", "AAPL", 1.0, 2.0, 0.5, 1.5, 100);
    dh.load_into_queue("2024-01-02", "AAPL", 2.0, 3.0, 1.5, 2.5, 200);
    dh.load_into_queue("2024-01-03", "AAPL", 3.0, 4.0, 2.5, 3.5, 300);
    EXPECT_EQ(dh.bar_count(), 3u);
    EXPECT_DOUBLE_EQ(dh.bar_at(0).close, 1.5);
    EXPECT_DOUBLE_EQ(dh.bar_at(2).close, 3.5);
}

TEST(DataHandler, HasBarData)
{
    // Use valid bar values (low > 0): the loader's input-validation guard
    // rejects rows with non-positive prices since they break downstream
    // mid/spread math. Earlier this test passed `low=0` which silently
    // dropped the row and left the handler empty.
    data_handler dh;
    dh.load_into_queue("2024-01-01", "X", 1.0, 2.0, 0.5, 1.5, 100);
    EXPECT_TRUE(dh.has_bar_data());
    EXPECT_FALSE(dh.has_tick_data());
}

TEST(DataHandler, HasTickData)
{
    data_handler dh;
    tick_record t;
    t.symbol = "X";
    t.price = 1.0;
    t.quantity = 1;
    t.side = data_tick_side::unknown;
    t.timestamp = std::chrono::system_clock::now();
    ASSERT_TRUE(dh.add_tick(t));
    EXPECT_TRUE(dh.has_tick_data());
    EXPECT_FALSE(dh.has_bar_data());
}

TEST(MarketSeries, OnBarOnTick)
{
    MarketSeries series;
    Bar b;
    b.date = "2024-01-01";
    b.symbol = "AAPL";
    b.open = 150; b.high = 155; b.low = 149; b.close = 153; b.volume = 1000;
    ASSERT_TRUE(series.on_bar(b));
    EXPECT_EQ(series.bar_count(), 1u);
    EXPECT_EQ(series.bar_at(0).symbol, "AAPL");
    EXPECT_DOUBLE_EQ(series.bar_at(0).close, 153.0);

    Tick t;
    t.symbol = "AAPL";
    t.price = 150.25;
    t.quantity = 10;
    t.side = data_tick_side::bid;
    t.timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(1704067200000));
    ASSERT_TRUE(series.on_tick(t));
    EXPECT_EQ(series.tick_count(), 1u);
    EXPECT_DOUBLE_EQ(series.tick_at(0).price, 150.25);
}

TEST(MarketSeries, ValidationRejectsNonPositive)
{
    SilenceOutput quiet;
    MarketSeries series;
    Bar b;
    b.symbol = "X";
    b.open = -1; b.high = 1; b.low = 0.5; b.close = 1; b.volume = 1;
    EXPECT_FALSE(series.on_bar(b));
    EXPECT_EQ(series.bar_count(), 0u);
    EXPECT_EQ(series.validation_errors(), 1u);
}

TEST(MarketSeries, ClearKeepsCapacityForMcReuse)
{
    MarketSeries series;
    series.reserve_bars(1000);
    series.load_into_queue("2024-01-01", "AAPL", 1, 2, 0.5, 1.5, 100);
    series.load_into_queue("2024-01-02", "AAPL", 2, 3, 1.5, 2.5, 200);
    EXPECT_EQ(series.bar_count(), 2u);
    series.clear();
    EXPECT_EQ(series.bar_count(), 0u);
    EXPECT_TRUE(series.empty());
    // Reload after clear (MC reuse path)
    series.load_into_queue("2024-01-03", "AAPL", 3, 4, 2.5, 3.5, 300);
    EXPECT_EQ(series.bar_count(), 1u);
}

TEST(MarketSeries, SortBarsByTimeMultiSymbol)
{
    MarketSeries series;
    series.load_into_queue("2024-01-02", "BBB", 1, 1, 1, 1, 1);
    series.load_into_queue("2024-01-01", "AAA", 1, 1, 1, 1, 1);
    series.load_into_queue("2024-01-01", "BBB", 1, 1, 1, 1, 1);
    series.sort_bars_by_time();
    EXPECT_EQ(series.bar_at(0).symbol, "AAA");
    EXPECT_EQ(series.bar_at(1).symbol, "BBB");
    EXPECT_EQ(std::string(series.bar_at(2).date), "2024-01-02");
}

TEST(CsvDataSource, LoadsCorrectly)
{
    SilenceOutput quiet;
    auto dh = std::make_shared<data_handler>();
    CsvDataSource src(fixture_path("sample_ohlcv.csv"));
    ASSERT_TRUE(src.load_data(dh));
    EXPECT_EQ(dh->bar_count(), 2u);
    EXPECT_DOUBLE_EQ(dh->bar_at(0).close, 153.0);
    EXPECT_DOUBLE_EQ(dh->bar_at(1).close, 157.0);
}

TEST(CsvDataSource, LoadIntoSink)
{
    SilenceOutput quiet;
    MarketSeries series;
    CsvDataSource src(fixture_path("sample_ohlcv.csv"));
    LoadStats stats;
    ASSERT_TRUE(src.load_into(series, &stats));
    EXPECT_EQ(stats.accepted, 2u);
    EXPECT_EQ(series.bar_count(), 2u);
}

TEST(CsvDataSource, FileNotFound)
{
    SilenceOutput quiet;
    auto dh = std::make_shared<data_handler>();
    CsvDataSource src("/nonexistent/path.csv");
    EXPECT_FALSE(src.load_data(dh));
}

TEST(TickCsvDataSource, LoadsCorrectly)
{
    SilenceOutput quiet;
    auto dh = std::make_shared<data_handler>();
    TickCsvDataSource src(fixture_path("sample_ticks.csv"));
    ASSERT_TRUE(src.load_data(dh));
    EXPECT_EQ(dh->tick_count(), 3u);
    EXPECT_DOUBLE_EQ(dh->tick_at(0).price, 150.25);
    EXPECT_DOUBLE_EQ(dh->tick_at(1).price, 150.30);
    EXPECT_EQ(dh->tick_at(2).quantity, 200);
}

TEST(TickCsvDataSource, SideParsing)
{
    SilenceOutput quiet;
    auto dh = std::make_shared<data_handler>();
    TickCsvDataSource src(fixture_path("sample_ticks.csv"));
    ASSERT_TRUE(src.load_data(dh));
    EXPECT_EQ(dh->tick_at(0).side, data_tick_side::bid);
    EXPECT_EQ(dh->tick_at(1).side, data_tick_side::ask);
    EXPECT_EQ(dh->tick_at(2).side, data_tick_side::bid);
}

TEST(TickCsvDataSource, FileNotFound)
{
    SilenceOutput quiet;
    auto dh = std::make_shared<data_handler>();
    TickCsvDataSource src("/nonexistent/ticks.csv");
    EXPECT_FALSE(src.load_data(dh));
}

TEST(DataWrapper, FromPathLoadsCsv)
{
    SilenceOutput quiet;
    MarketSeries series;
    auto w = DataWrapper::from_path(fixture_path("sample_ohlcv.csv"));
    ASSERT_TRUE(w.load(series));
    EXPECT_EQ(series.bar_count(), 2u);
    EXPECT_DOUBLE_EQ(series.bar_at(0).close, 153.0);
}

TEST(DataWrapper, FromUriCsvScheme)
{
    SilenceOutput quiet;
    MarketSeries series;
    auto uri = std::string("csv:") + fixture_path("sample_ohlcv.csv");
    auto w = DataWrapper::from_uri(uri);
    ASSERT_TRUE(w.load(series));
    EXPECT_EQ(series.bar_count(), 2u);
}
