#include <gtest/gtest.h>
#include "helpers/mock_transport.h"
#include "providers/data_bridge.h"
#include "providers/binance/binance_combined_parser.h"
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

class FailingStreamingTransport final : public IDataTransport
{
public:
	bool open() override { open_ = true; return true; }
	void close() override { open_ = false; }
	bool is_open() const override { return open_; }
	bool is_streaming() const override { return true; }
	std::optional<std::string> read_line() override { return std::nullopt; }
	std::optional<std::string> read_line_blocking() override
	{
		if (!header_sent_)
		{
			header_sent_ = true;
			return "date,symbol,open,high,low,close,volume";
		}
		open_ = false;
		return std::nullopt;
	}
	transport_terminal_status terminal_status() const override
	{
		return header_sent_ ? transport_terminal_status::failed
		                    : transport_terminal_status::unknown;
	}
private:
	bool open_ = false;
	bool header_sent_ = false;
};

class FrameBarParser final : public IDataParser<bar_record>
{
public:
	bool parse_header(const std::string&) override { return true; }
	bool header_frame_contains_records() const override { return true; }
	std::optional<bar_record> parse_record(const std::string& line) override
	{
		if (line != "record") return std::nullopt;
		bar_record out;
		out.date = "2024-01-01";
		out.symbol = "TEST";
		out.open = out.high = out.low = out.close = 1.0;
		return out;
	}
	empty_parse_status classify_empty_frame(std::string_view frame) const override
	{
		return (frame == "control" || frame == "header")
			? empty_parse_status::ignored
		                          : empty_parse_status::malformed;
	}
};

class TimedIdleTransport final : public IDataTransport
{
public:
	bool open() override { open_ = true; return true; }
	void close() override { open_ = false; }
	bool is_open() const override { return open_; }
	bool is_streaming() const override { return true; }
	std::optional<std::string> read_line() override { return std::nullopt; }
	bool supports_bounded_idle_read() const override { return true; }
	transport_read_result read_frame_until(
		std::string_view& out,
		std::chrono::steady_clock::time_point) override
	{
		if (reads_++ == 0)
		{
			frame_ = "header";
			out = frame_;
			return transport_read_result::frame;
		}
		if (reads_ <= 3) return transport_read_result::idle;
		open_ = false;
		return transport_read_result::terminal;
	}
	transport_terminal_status terminal_status() const override
	{
		return transport_terminal_status::clean_eof;
	}
private:
	bool open_ = false;
	int reads_ = 0;
	std::string frame_;
};
}

// --- Batch mode tests ---

TEST(DataBridge, ConstructorRejectsMissingTransportOrParser)
{
	auto transport = std::make_shared<MockBatchTransport>(
		std::vector<std::string>{});
	auto parser = std::make_shared<CsvBarParser>();

	EXPECT_THROW(
		(DataBridge<bar_record>{nullptr, parser, bar_record_sink}),
		std::invalid_argument);
	EXPECT_THROW(
		(DataBridge<bar_record>{transport, nullptr, bar_record_sink}),
		std::invalid_argument);
}

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
	LoadStats stats;
	ASSERT_TRUE(bridge->load_into(*dh, &stats));
	EXPECT_EQ(dh->bar_count(), 2u);
	// DR-REPLAY-02: unparseable frames must be accounted (not silently dropped).
	EXPECT_EQ(stats.accepted, 2u);
	EXPECT_EQ(stats.rejected, 2u);
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

TEST(DataBridge, CsvTicksPreserveFractionalQuantityScale)
{
	SilenceBridge quiet;
	auto transport = std::make_shared<MockBatchTransport>(std::vector<std::string>{
		"timestamp_ms,symbol,price,quantity,side",
		"1704067200000,BTCUSDT,42000.0,0.25,B",
		"1704067200010,BTCUSDT,42001.0,2,A",
	});
	auto parser = std::make_shared<CsvTickParser>();
	auto bridge = std::make_shared<DataBridge<tick_record>>(
		transport, parser, tick_record_sink);

	auto dh = std::make_shared<data_handler>();
	ASSERT_TRUE(bridge->load_data(dh));
	ASSERT_EQ(dh->tick_count(), 2u);
	EXPECT_EQ(dh->tick_at(0).quantity, 25'000'000);
	EXPECT_EQ(dh->tick_at(0).quantity_scale, 100'000'000ULL);
	EXPECT_EQ(dh->tick_at(1).quantity, 2);
	EXPECT_EQ(dh->tick_at(1).quantity_scale, 1u);
}

TEST(DataBridge, CsvTickRejectsInvalidOrOverflowQuantity)
{
	CsvTickParser parser;
	EXPECT_FALSE(parser.parse_record(
		"1704067200000,BTCUSDT,42000.0,-0.1,B"));
	EXPECT_FALSE(parser.parse_record(
		"1704067200000,BTCUSDT,42000.0,1e100,B"));
	EXPECT_FALSE(parser.parse_record(
		"1704067200000,BTCUSDT,42000.0,nan,B"));
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
	auto result = bridge->run_streaming(dh);
	feeder.join();

	EXPECT_EQ(dh->bar_count(), 10u);
	EXPECT_TRUE(result.success());
	EXPECT_EQ(result.termination, stream_termination::operator_stop);
}

TEST(DataBridge, BinanceFormingKlineDoesNotTerminateBeforeClosedKline)
{
	SilenceBridge quiet;
	auto transport = std::make_shared<MockStreamingTransport>();
	auto parser = std::make_shared<BinanceCombinedParser>();
	auto bridge = std::make_shared<DataBridge<provider::event>>(transport, parser);
	auto dh = std::make_shared<data_handler>();

	const std::string forming =
		R"({"stream":"btcusdt@kline_1m","data":)"
		R"({"e":"kline","E":1,"s":"BTCUSDT","k":)"
		R"({"t":1000,"s":"BTCUSDT","o":"100","h":"102",)"
		R"("l":"99","c":"101","v":"2","x":false}}})";
	const std::string closed =
		R"({"stream":"btcusdt@kline_1m","data":)"
		R"({"e":"kline","E":2,"s":"BTCUSDT","k":)"
		R"({"t":1000,"s":"BTCUSDT","o":"100","h":"103",)"
		R"("l":"99","c":"102","v":"3","x":true}}})";

	transport->enqueue(forming); // first streaming frame is also parsed
	transport->enqueue(closed);
	std::thread stopper([&] {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		transport->request_stop();
	});
	bridge->set_retain_streamed(true);
	const auto result = bridge->run_streaming(dh);
	stopper.join();

	ASSERT_TRUE(result.success());
	EXPECT_EQ(result.rejected, 0u);
	ASSERT_EQ(dh->bar_count(), 1u);
	EXPECT_DOUBLE_EQ(dh->bar_at(0).close, 102.0);
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

	auto result = bridge->run_streaming(
		dh, [&](const bar_record&) { callback_count++; });
	feeder.join();

	EXPECT_EQ(callback_count.load(), 5);
	EXPECT_EQ(dh->bar_count(), 0u); // retain_streamed=false default
	EXPECT_TRUE(result.success());
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
		auto result = bridge->run_streaming(dh);
		EXPECT_EQ(result.termination, stream_termination::operator_stop);
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

TEST(DataBridge, StreamingCallbackExceptionStopsAndClosesTransport)
{
	SilenceBridge quiet;
	auto transport = std::make_shared<MockStreamingTransport>();
	auto bridge = std::make_shared<DataBridge<bar_record>>(
		transport, std::make_shared<CsvBarParser>(), bar_record_sink);
	transport->enqueue("date,symbol,open,high,low,close,volume");
	transport->enqueue("2024-01-01,TEST,100,105,95,102,1000");

	const auto result = bridge->run_streaming(
		std::make_shared<data_handler>(), [](const bar_record&) {
			throw std::runtime_error("callback failure");
		});

	EXPECT_FALSE(result.success());
	EXPECT_EQ(result.termination, stream_termination::runtime_failure);
	EXPECT_FALSE(transport->is_open());
}

TEST(DataBridge, StreamingTransportFailureIsNotFalseCompletion)
{
	SilenceBridge quiet;
	auto transport = std::make_shared<FailingStreamingTransport>();
	auto parser = std::make_shared<CsvBarParser>();
	auto bridge = std::make_shared<DataBridge<bar_record>>(
		transport, parser, bar_record_sink);
	auto result = bridge->run_streaming(std::make_shared<data_handler>());
	EXPECT_FALSE(result.success());
	EXPECT_EQ(result.termination, stream_termination::transport_failure);
}

TEST(DataBridge, StreamingMalformedFrameIsFailureButControlFrameIsIgnored)
{
	SilenceBridge quiet;
	auto transport = std::make_shared<MockStreamingTransport>();
	auto parser = std::make_shared<FrameBarParser>();
	auto bridge = std::make_shared<DataBridge<bar_record>>(
		transport, parser, bar_record_sink);
	transport->enqueue("control");
	transport->enqueue("record");
	transport->enqueue("malformed");
	transport->request_stop();
	auto result = bridge->run_streaming(std::make_shared<data_handler>());
	EXPECT_FALSE(result.success());
	EXPECT_EQ(result.termination, stream_termination::parse_or_sink_failure);
	EXPECT_EQ(result.accepted, 1u);
	EXPECT_EQ(result.rejected, 1u);
}

TEST(DataBridge, StreamingMalformedFirstFrameTerminatesWithoutWaitingForMoreInput)
{
	SilenceBridge quiet;
	auto transport = std::make_shared<MockStreamingTransport>();
	auto bridge = std::make_shared<DataBridge<bar_record>>(
		transport, std::make_shared<FrameBarParser>(), bar_record_sink);
	transport->enqueue("malformed");
	auto result = bridge->run_streaming(std::make_shared<data_handler>());
	EXPECT_FALSE(result.success());
	EXPECT_EQ(result.termination, stream_termination::parse_or_sink_failure);
	EXPECT_EQ(result.accepted, 0u);
	EXPECT_EQ(result.rejected, 1u);
}

TEST(DataBridge, PrelatchedStopIsNotClearedAtStreamingEntry)
{
	SilenceBridge quiet;
	auto transport = std::make_shared<MockStreamingTransport>();
	auto bridge = std::make_shared<DataBridge<bar_record>>(
		transport, std::make_shared<FrameBarParser>(), bar_record_sink);
	bridge->stop();
	auto result = bridge->run_streaming(std::make_shared<data_handler>());
	EXPECT_TRUE(result.success());
	EXPECT_EQ(result.termination, stream_termination::operator_stop);
	EXPECT_EQ(result.accepted, 0u);
}

TEST(DataBridge, PrelatchedHaltDoesNotReopenTransport)
{
	SilenceBridge quiet;
	auto transport = std::make_shared<MockStreamingTransport>();
	auto bridge = std::make_shared<DataBridge<bar_record>>(
		transport, std::make_shared<FrameBarParser>(), bar_record_sink);
	std::atomic<bool> halt{true};
	auto result = bridge->run_streaming(
		std::make_shared<data_handler>(), nullptr, nullptr, &halt);
	EXPECT_FALSE(result.success());
	EXPECT_EQ(result.termination, stream_termination::engine_halt);
	EXPECT_FALSE(transport->is_open());
}

TEST(DataBridge, ConcurrentHaltWakesBlockingStreamAsEngineHalt)
{
	SilenceBridge quiet;
	auto transport = std::make_shared<MockStreamingTransport>();
	auto bridge = std::make_shared<DataBridge<bar_record>>(
		transport, std::make_shared<CsvBarParser>(), bar_record_sink);
	std::atomic<bool> halt{false};
	transport->enqueue("date,symbol,open,high,low,close,volume");

	StreamResult result;
	std::thread runner([&] {
		result = bridge->run_streaming(
			std::make_shared<data_handler>(), nullptr, nullptr, &halt);
	});
	transport->wait_for_blocking_reads(2);
	const auto start = std::chrono::steady_clock::now();
	halt.store(true, std::memory_order_release);
	transport->request_stop();
	runner.join();

	EXPECT_EQ(result.termination, stream_termination::engine_halt);
	EXPECT_FALSE(result.success());
	EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - start).count(), 500);
}

TEST(DataBridge, BoundedIdleReadInvokesControlCallbackOnDrivingThread)
{
	SilenceBridge quiet;
	auto transport = std::make_shared<TimedIdleTransport>();
	auto bridge = std::make_shared<DataBridge<bar_record>>(
		transport, std::make_shared<FrameBarParser>(), bar_record_sink);
	const auto caller = std::this_thread::get_id();
	int idle_calls = 0;
	auto result = bridge->run_streaming(
		std::make_shared<data_handler>(), nullptr, [&] {
			EXPECT_EQ(std::this_thread::get_id(), caller);
			++idle_calls;
		});
	EXPECT_TRUE(result.success());
	EXPECT_EQ(idle_calls, 2);
}

TEST(DataBridge, IdleCallbackRefusesTransportWithoutBoundedRead)
{
	SilenceBridge quiet;
	auto transport = std::make_shared<MockStreamingTransport>();
	auto bridge = std::make_shared<DataBridge<bar_record>>(
		transport, std::make_shared<FrameBarParser>(), bar_record_sink);
	auto result = bridge->run_streaming(
		std::make_shared<data_handler>(), nullptr, [] {});
	EXPECT_FALSE(result.success());
	EXPECT_EQ(result.termination, stream_termination::transport_open_failure);
	EXPECT_FALSE(transport->is_open());
}
