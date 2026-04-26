#include <gtest/gtest.h>
#include "data/data_handler.h"
#include "data/csv_data_source.h"
#include "data/tick_csv_data_source.h"
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
} // namespace

// Helper: path to test fixtures relative to the test binary
// We use an absolute path since ctest may run from build dir
static std::string fixture_path(const std::string& name)
{
    // Try relative to source root
    std::string p = std::string(TEST_FIXTURES_DIR) + "/" + name;
    return p;
}

TEST(DataHandler, LoadIntoQueue)
{
    data_handler dh;
    dh.load_into_queue("2024-01-01", "AAPL", 1.0, 2.0, 0.5, 1.5, 100);
    dh.load_into_queue("2024-01-02", "AAPL", 2.0, 3.0, 1.5, 2.5, 200);
    dh.load_into_queue("2024-01-03", "AAPL", 3.0, 4.0, 2.5, 3.5, 300);
    EXPECT_EQ(dh.db_data_symbol.size(), 3u);
    EXPECT_EQ(dh.db_data_close_value.size(), 3u);
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
    dh.tick_data.push_back(t);
    EXPECT_TRUE(dh.has_tick_data());
    EXPECT_FALSE(dh.has_bar_data());
}

TEST(CsvDataSource, LoadsCorrectly)
{
    SilenceOutput quiet;
    auto dh = std::make_shared<data_handler>();
    CsvDataSource src(fixture_path("sample_ohlcv.csv"));
    ASSERT_TRUE(src.load_data(dh));
    EXPECT_EQ(dh->db_data_symbol.size(), 2u);
    EXPECT_DOUBLE_EQ(dh->db_data_close_value[0], 153.0);
    EXPECT_DOUBLE_EQ(dh->db_data_close_value[1], 157.0);
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
    EXPECT_EQ(dh->tick_data.size(), 3u);
    EXPECT_DOUBLE_EQ(dh->tick_data[0].price, 150.25);
    EXPECT_DOUBLE_EQ(dh->tick_data[1].price, 150.30);
    EXPECT_EQ(dh->tick_data[2].quantity, 200);
}

TEST(TickCsvDataSource, SideParsing)
{
    SilenceOutput quiet;
    auto dh = std::make_shared<data_handler>();
    TickCsvDataSource src(fixture_path("sample_ticks.csv"));
    ASSERT_TRUE(src.load_data(dh));
    EXPECT_EQ(dh->tick_data[0].side, data_tick_side::bid);
    EXPECT_EQ(dh->tick_data[1].side, data_tick_side::ask);
    EXPECT_EQ(dh->tick_data[2].side, data_tick_side::bid);
}

TEST(TickCsvDataSource, FileNotFound)
{
    SilenceOutput quiet;
    auto dh = std::make_shared<data_handler>();
    TickCsvDataSource src("/nonexistent/ticks.csv");
    EXPECT_FALSE(src.load_data(dh));
}
