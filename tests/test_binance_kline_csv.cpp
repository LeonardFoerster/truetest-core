#include <gtest/gtest.h>

#include "data/csv_data_source.h"
#include "data/data_handler.h"
#include "providers/data_bridge.h"
#include "providers/local/csv_parser.h"
#include "providers/local/file_transport.h"
#include "helpers/mock_transport.h"

#include <chrono>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct SilenceOut {
	std::ostringstream sink;
	std::streambuf* orig_out;
	std::streambuf* orig_err;
	SilenceOut()
		: orig_out(std::cout.rdbuf(sink.rdbuf()))
		, orig_err(std::cerr.rdbuf(sink.rdbuf())) {}
	~SilenceOut() {
		std::cout.rdbuf(orig_out);
		std::cerr.rdbuf(orig_err);
	}
};

std::string fixture_path(const std::string& name)
{
	return std::string(TEST_FIXTURES_DIR) + "/" + name;
}

} // namespace

TEST(BinanceKlineCsv, ParseVolumeIntegerPassthrough)
{
	EXPECT_EQ(tt::csv::parse_bar_volume("1000000"), 1000000);
	EXPECT_EQ(tt::csv::parse_bar_volume(""), 0);
}

TEST(BinanceKlineCsv, ParseVolumeFractionalScaled)
{
	// 246.092 * 1e8 → 24609200000
	EXPECT_EQ(tt::csv::parse_bar_volume("246.092"), 24609200000LL);
	EXPECT_EQ(tt::csv::parse_bar_volume("100.5"), 10050000000LL);
}

TEST(BinanceKlineCsv, CsvBarParserOpenTimeAndVolume)
{
	SilenceOut quiet;
	CsvBarParser parser;
	ASSERT_TRUE(parser.parse_header(
		"date,open_time,open,high,low,close,volume,close_time,quote_asset_volume,"
		"number_of_trades,taker_buy_base_asset_volume,taker_buy_quote_asset_volume,ignore"));

	const std::string line =
		"2020-01-01,1577836800000,7189.43,7190.52,7177,7182.44,246.092,"
		"1577836859999,1767430.16121,336,46.630,334813.19820,0";
	auto rec = parser.parse_record(line);
	ASSERT_TRUE(rec.has_value());
	EXPECT_EQ(rec->open_time_ms, 1577836800000LL);
	EXPECT_EQ(rec->volume, 24609200000LL);
	EXPECT_DOUBLE_EQ(rec->open, 7189.43);
	EXPECT_DOUBLE_EQ(rec->close, 7182.44);
}

TEST(BinanceKlineCsv, FixtureLoadViaDataBridge)
{
	SilenceOut quiet;
	const auto path = fixture_path("binance_kline_sample.csv");
	auto transport = std::make_shared<FileTransport>(path);
	auto parser = std::make_shared<CsvBarParser>();
	auto bridge = std::make_shared<DataBridge<bar_record>>(transport, parser, bar_record_sink);

	auto dh = std::make_shared<data_handler>();
	ASSERT_TRUE(bridge->load_data(dh));
	// header + 20 data rows in fixture
	EXPECT_EQ(dh->bar_count(), 20u);

	const auto b0 = dh->bar_at(0);
	const auto b1 = dh->bar_at(1);
	EXPECT_GT(b0.volume, 0);
	EXPECT_EQ(b0.volume, 24609200000LL);

	using ms = std::chrono::milliseconds;
	const auto t0 = std::chrono::duration_cast<ms>(b0.ts.time_since_epoch()).count();
	const auto t1 = std::chrono::duration_cast<ms>(b1.ts.time_since_epoch()).count();
	EXPECT_EQ(t0, 1577836800000LL);
	EXPECT_EQ(t1 - t0, 60000); // 1m bars
}

TEST(BinanceKlineCsv, FixtureLoadViaCsvDataSource)
{
	SilenceOut quiet;
	const auto path = fixture_path("binance_kline_sample.csv");
	CsvDataSource src(path);
	data_handler dh;
	ASSERT_TRUE(src.load_into(dh, nullptr));
	EXPECT_EQ(dh.bar_count(), 20u);
	EXPECT_EQ(dh.bar_at(0).volume, 24609200000LL);
	using ms = std::chrono::milliseconds;
	const auto t0 = std::chrono::duration_cast<ms>(dh.bar_at(0).ts.time_since_epoch()).count();
	EXPECT_EQ(t0, 1577836800000LL);
}
