#include <gtest/gtest.h>
#include "test_helpers/mock_transport.h"
#include "providers/data_bridge.h"
#include "providers/local/csv_parser.h"
#include "data/data_handler.h"

#include <thread>
#include <chrono>
#include <atomic>
#include <sstream>

// RAII helper to silence stdout/stderr.
// Members declared in init order to avoid using `sink` before construction.
namespace {
struct SilenceBridge {
	std::ostringstream sink;
	std::streambuf* orig_out;
	std::streambuf* orig_err;
	SilenceBridge()
		: sink()
		, orig_out(std::cout.rdbuf(sink.rdbuf()))
		, orig_err(std::cerr.rdbuf(sink.rdbuf())) {}
	~SilenceBridge() {
		std::cout.rdbuf(orig_out);
		std::cerr.rdbuf(orig_err);
	}
};
}

// --- Batch mode tests ---

TEST(DataBridge, BatchLoadsBarRecords)
{
	SilenceBridge quiet;
	auto transport = std::make_shared<MockBatchTransport>(std::vector<std::string>{
		"date,symbol,open,high,low,close,volume",
		"2024-01-01,AAPL,150.0,155.0,149.0,153.0,1000000",
		"2024-01-02,AAPL,153.0,158.0,152.0,157.0,1200000",
		"2024-01-03,AAPL,157.0,160.0,155.0,159.0,900000",
		"2024-01-04,AAPL,159.0,162.0,157.0,161.0,800000",
		"2024-01-05,AAPL,161.0,165.0,160.0,164.0,1100000",
	});
	auto parser = std::make_shared<CsvBarParser>();
	auto bridge = std::make_shared<DataBridge<bar_record>>(transport, parser, bar_record_sink);

	auto dh = std::make_shared<data_handler>();
	ASSERT_TRUE(bridge->load_data(dh));
	EXPECT_EQ(dh->bar_count(), 5u);
	EXPECT_DOUBLE_EQ(dh->bar_at(0).close, 153.0);
	EXPECT_DOUBLE_EQ(dh->bar_at(4).close, 164.0);
}

TEST(DataBridge, BatchEmptyTransportReturnsFalse)
{
	SilenceBridge quiet;
	auto transport = std::make_shared<MockBatchTransport>(std::vector<std::string>{});
	auto parser = std::make_shared<CsvBarParser>();
	auto bridge = std::make_shared<DataBridge<bar_record>>(transport, parser, bar_record_sink);

	auto dh = std::make_shared<data_handler>();
	EXPECT_FALSE(bridge->load_data(dh));
}

TEST(DataBridge, BatchHeaderOnlyReturnsFalse)
{
	SilenceBridge quiet;
	auto transport = std::make_shared<MockBatchTransport>(std::vector<std::string>{
		"date,symbol,open,high,low,close,volume",
	});
	auto parser = std::make_shared<CsvBarParser>();
	auto bridge = std::make_shared<DataBridge<bar_record>>(transport, parser, bar_record_sink);

	auto dh = std::make_shared<data_handler>();
	EXPECT_FALSE(bridge->load_data(dh));
}

TEST(DataBridge, BatchSkipsMalformedLines)
{
	SilenceBridge quiet;
	auto transport = std::make_shared<MockBatchTransport>(std::vector<std::string>{
		"date,symbol,open,high,low,close,volume",
		"2024-01-01,AAPL,150.0,155.0,149.0,153.0,1000000",
		"this,is,not,valid",
		"",
		"2024-01-02,AAPL,153.0,158.0,152.0,157.0,1200000",
	});
	auto parser = std::make_shared<CsvBarParser>();
	auto bridge = std::make_shared<DataBridge<bar_record>>(transport, parser, bar_record_sink);

	auto dh = std::make_shared<data_handler>();
	ASSERT_TRUE(bridge->load_data(dh));
	EXPECT_EQ(dh->bar_count(), 2u);
}

TEST(DataBridge, BatchLoadsTickRecords)
{
	SilenceBridge quiet;
	auto transport = std::make_shared<MockBatchTransport>(std::vector<std::string>{
		"timestamp_ms,symbol,price,quantity,side",
		"1704067200000,AAPL,150.25,100,B",
		"1704067200010,AAPL,150.30,50,A",
	});
	auto parser = std::make_shared<CsvTickParser>();
	auto bridge = std::make_shared<DataBridge<tick_record>>(transport, parser, tick_record_sink);

	auto dh = std::make_shared<data_handler>();
	ASSERT_TRUE(bridge->load_data(dh));
	EXPECT_EQ(dh->tick_count(), 2u);
	EXPECT_DOUBLE_EQ(dh->tick_at(0).price, 150.25);
	EXPECT_EQ(dh->tick_at(1).side, data_tick_side::ask);
}

// --- Streaming mode tests ---

TEST(DataBridge, StreamingDeliversAllRecords)
{
	SilenceBridge quiet;
	auto transport = std::make_shared<MockStreamingTransport>();
	auto parser = std::make_shared<CsvBarParser>();
	auto bridge = std::make_shared<DataBridge<bar_record>>(transport, parser, bar_record_sink);

	auto dh = std::make_shared<data_handler>();

	std::thread feeder([&] {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		// First line is treated as header by run_streaming
		transport->enqueue("date,symbol,open,high,low,close,volume");
		for (int i = 0; i < 10; ++i)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			transport->enqueue("2024-01-01,TEST," +
				std::to_string(100.0 + i) + "," +
				std::to_string(105.0 + i) + "," +
				std::to_string(95.0 + i) + "," +
				std::to_string(102.0 + i) + ",1000");
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		transport->request_stop();
	});

	// D-06: retain_streamed defaults false — enable to assert series growth.
	bridge->set_retain_streamed(true);
	bridge->run_streaming(dh);
	feeder.join();

	EXPECT_EQ(dh->bar_count(), 10u);
}

TEST(DataBridge, StreamingDoesNotRetainByDefault)
{
	SilenceBridge quiet;
	auto transport = std::make_shared<MockStreamingTransport>();
	auto parser = std::make_shared<CsvBarParser>();
	auto bridge = std::make_shared<DataBridge<bar_record>>(transport, parser, bar_record_sink);

	auto dh = std::make_shared<data_handler>();
	std::atomic<int> callback_count{0};

	std::thread feeder([&] {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		transport->enqueue("date,symbol,open,high,low,close,volume");
		for (int i = 0; i < 5; ++i)
			transport->enqueue("2024-01-01,TEST,100,105,95,102,1000");
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		transport->request_stop();
	});

	bridge->run_streaming(dh, [&](const bar_record&) { callback_count++; });
	feeder.join();

	EXPECT_EQ(callback_count.load(), 5);
	EXPECT_EQ(dh->bar_count(), 0u); // retain_streamed=false default
}

TEST(DataBridge, StreamingStopReturnPromptly)
{
	SilenceBridge quiet;
	auto transport = std::make_shared<MockStreamingTransport>();
	auto parser = std::make_shared<CsvBarParser>();
	auto bridge = std::make_shared<DataBridge<bar_record>>(transport, parser, bar_record_sink);

	auto dh = std::make_shared<data_handler>();
	std::atomic<bool> done{false};

	std::thread runner([&] {
		// Enqueue header so run_streaming can start
		transport->enqueue("date,symbol,open,high,low,close,volume");
		bridge->run_streaming(dh);
		done = true;
	});

	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	EXPECT_FALSE(done.load());

	auto t0 = std::chrono::steady_clock::now();
	bridge->stop();
	runner.join();
	auto elapsed = std::chrono::steady_clock::now() - t0;

	EXPECT_TRUE(done.load());
	EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 500);
}

TEST(DataBridge, StreamingCallbackFiresPerRecord)
{
	SilenceBridge quiet;
	auto transport = std::make_shared<MockStreamingTransport>();
	auto parser = std::make_shared<CsvBarParser>();
	auto bridge = std::make_shared<DataBridge<bar_record>>(transport, parser, bar_record_sink);

	auto dh = std::make_shared<data_handler>();
	std::atomic<int> callback_count{0};

	std::thread feeder([&] {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		transport->enqueue("date,symbol,open,high,low,close,volume");
		for (int i = 0; i < 5; ++i)
		{
			transport->enqueue("2024-01-01,TEST,100,105,95,102,1000");
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		transport->request_stop();
	});

	bridge->run_streaming(dh, [&](const bar_record&) {
		callback_count++;
	});
	feeder.join();

	EXPECT_EQ(callback_count.load(), 5);
}
