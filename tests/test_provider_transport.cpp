#include <gtest/gtest.h>
#include "test_helpers/mock_transport.h"
#include "providers/local/file_transport.h"

#include <thread>
#include <chrono>
#include <atomic>

static std::string fixture_path(const std::string& name)
{
	return std::string(TEST_FIXTURES_DIR) + "/" + name;
}

// --- FileTransport (batch) tests ---

TEST(FileTransport, IsStreamingReturnsFalse)
{
	FileTransport ft(fixture_path("sample_ohlcv.csv"));
	EXPECT_FALSE(ft.is_streaming());
}

TEST(FileTransport, ReadLineBlockingFallsBackToReadLine)
{
	FileTransport ft(fixture_path("sample_ohlcv.csv"));
	ASSERT_TRUE(ft.open());

	// read_line_blocking() should behave identically to read_line()
	auto line1 = ft.read_line_blocking();
	ASSERT_TRUE(line1.has_value());
	EXPECT_FALSE(line1->empty());  // header line

	auto line2 = ft.read_line_blocking();
	ASSERT_TRUE(line2.has_value());

	auto line3 = ft.read_line_blocking();
	ASSERT_TRUE(line3.has_value());

	// EOF
	auto line4 = ft.read_line_blocking();
	EXPECT_FALSE(line4.has_value());

	ft.close();
}

TEST(FileTransport, RequestStopCallsClose)
{
	FileTransport ft(fixture_path("sample_ohlcv.csv"));
	ASSERT_TRUE(ft.open());
	EXPECT_TRUE(ft.is_open());

	ft.request_stop();
	EXPECT_FALSE(ft.is_open());
}

// --- MockStreamingTransport tests ---

TEST(MockStreamingTransport, IsStreamingReturnsTrue)
{
	MockStreamingTransport mt;
	EXPECT_TRUE(mt.is_streaming());
}

TEST(MockStreamingTransport, ReadLineBlockingBlocksUntilData)
{
	auto mt = std::make_shared<MockStreamingTransport>();
	mt->open();

	std::optional<std::string> result;
	std::thread reader([&] {
		result = mt->read_line_blocking();
	});

	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	mt->enqueue("hello");
	reader.join();

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(*result, "hello");
}

TEST(MockStreamingTransport, RequestStopUnblocksReader)
{
	auto mt = std::make_shared<MockStreamingTransport>();
	mt->open();

	std::optional<std::string> result;
	std::atomic<bool> done{false};

	std::thread reader([&] {
		result = mt->read_line_blocking();
		done = true;
	});

	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	EXPECT_FALSE(done.load());  // still blocked

	mt->request_stop();
	reader.join();

	EXPECT_TRUE(done.load());
	EXPECT_FALSE(result.has_value());  // nullopt on stop
}

TEST(MockStreamingTransport, ReturnsDataInOrder)
{
	MockStreamingTransport mt;
	mt.open();
	mt.enqueue("line1");
	mt.enqueue("line2");
	mt.enqueue("line3");

	EXPECT_EQ(mt.read_line_blocking().value(), "line1");
	EXPECT_EQ(mt.read_line_blocking().value(), "line2");
	EXPECT_EQ(mt.read_line_blocking().value(), "line3");
}

TEST(MockStreamingTransport, StopWithPendingDataDrainsFirst)
{
	MockStreamingTransport mt;
	mt.open();
	mt.enqueue("data");
	mt.request_stop();

	// Data enqueued before stop should still be readable
	auto result = mt.read_line_blocking();
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(*result, "data");
}

// --- MockBatchTransport tests ---

TEST(MockBatchTransport, IsStreamingReturnsFalse)
{
	MockBatchTransport mt({});
	EXPECT_FALSE(mt.is_streaming());
}

TEST(MockBatchTransport, ReturnsLinesInOrder)
{
	MockBatchTransport mt({"a", "b", "c"});
	mt.open();
	EXPECT_EQ(mt.read_line().value(), "a");
	EXPECT_EQ(mt.read_line().value(), "b");
	EXPECT_EQ(mt.read_line().value(), "c");
	EXPECT_FALSE(mt.read_line().has_value());
}
