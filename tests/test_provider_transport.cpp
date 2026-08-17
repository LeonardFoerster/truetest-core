#include <gtest/gtest.h>
#include "helpers/mock_transport.h"
#include "providers/local/file_transport.h"
#include "providers/binance/binance_recorder.h"
#include "providers/bounded_ws_frame_reader.h"
#include "providers/socket_readiness.h"
#include "helpers/alloc_counter.h"

#include <boost/asio/buffer.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/local/connect_pair.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/beast/websocket.hpp>

#include <algorithm>
#include <cerrno>
#include <memory>
#include <thread>
#include <chrono>
#include <atomic>
#include <functional>

namespace {

class FakeAsyncWebSocket
{
public:
    explicit FakeAsyncWebSocket(boost::asio::io_context& ioc) : ioc_(ioc) {}

    template<class DynamicBuffer, class Handler>
    void async_read(DynamicBuffer& buffer, Handler&& handler)
    {
        ++start_count_;
        buffer_ = &buffer;
        handler_ = std::forward<Handler>(handler);
    }

    std::size_t start_count() const noexcept { return start_count_; }

    void complete(std::string_view payload,
                  boost::beast::error_code ec = {})
    {
        ASSERT_NE(buffer_, nullptr);
        auto destination = buffer_->prepare(payload.size());
        boost::asio::buffer_copy(destination, boost::asio::buffer(payload));
        buffer_->commit(payload.size());
        auto handler = std::move(handler_);
        boost::asio::post(ioc_,
            [handler = std::move(handler), ec, size = payload.size()]() mutable {
                handler(ec, size);
            });
    }

private:
    boost::asio::io_context& ioc_;
    boost::beast::flat_buffer* buffer_ = nullptr;
    std::function<void(boost::beast::error_code, std::size_t)> handler_;
    std::size_t start_count_ = 0;
};

} // namespace

TEST(BoundedWsFrameReader, PendingPartialFrameReturnsAtDeadlineAndResumes)
{
    auto ioc = std::make_shared<boost::asio::io_context>();
    auto websocket = std::make_shared<FakeAsyncWebSocket>(*ioc);
    provider_ws::BoundedFrameReader<FakeAsyncWebSocket> reader;
    std::string_view frame;

    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(reader.read_until(ioc, websocket, frame,
                               start + std::chrono::milliseconds{10}),
              transport_read_result::idle);
    EXPECT_TRUE(reader.pending());
    EXPECT_LT(std::chrono::steady_clock::now() - start,
              std::chrono::milliseconds{100});

    websocket->complete(R"({"stream":"trade"})");
    EXPECT_EQ(reader.read_until(
                  ioc, websocket, frame,
                  std::chrono::steady_clock::now() + std::chrono::milliseconds{100}),
              transport_read_result::frame);
    EXPECT_EQ(frame, R"({"stream":"trade"})");
    EXPECT_FALSE(reader.pending());
}

TEST(BoundedWsFrameReader, LateCancellationCompletionOutlivesReaderSafely)
{
    auto ioc = std::make_shared<boost::asio::io_context>();
    auto websocket = std::make_shared<FakeAsyncWebSocket>(*ioc);
    {
        provider_ws::BoundedFrameReader<FakeAsyncWebSocket> reader;
        std::string_view frame;
        EXPECT_EQ(reader.read_until(
                      ioc, websocket, frame,
                      std::chrono::steady_clock::now()
                          + std::chrono::milliseconds{1}),
                  transport_read_result::idle);
        EXPECT_FALSE(reader.drain_after_cancel(
            ioc, std::chrono::steady_clock::now()
                + std::chrono::milliseconds{1}));
    }

    websocket->complete({}, boost::asio::error::operation_aborted);
    ioc->restart();
    ioc->run();
    EXPECT_EQ(websocket->start_count(), 1U);
}

TEST(BoundedWsFrameReader, RealBeastNativeShutdownCompletesAfterReaderDestruction)
{
    namespace websocket = boost::beast::websocket;
    using local_socket = boost::asio::local::stream_protocol::socket;
    using test_websocket = websocket::stream<local_socket>;
    auto ioc = std::make_shared<boost::asio::io_context>();
    auto client = std::make_shared<test_websocket>(*ioc);
    test_websocket server(*ioc);
    boost::asio::local::connect_pair(
        client->next_layer(), server.next_layer());

    boost::beast::error_code client_ec;
    boost::beast::error_code server_ec;
    client->async_handshake("localhost", "/",
        [&](boost::beast::error_code ec) { client_ec = ec; });
    server.async_accept(
        [&](boost::beast::error_code ec) { server_ec = ec; });
    ioc->run();
    ASSERT_FALSE(client_ec);
    ASSERT_FALSE(server_ec);

    auto reader = std::make_unique<
        provider_ws::BoundedFrameReader<test_websocket>>();
    std::string_view frame;
    EXPECT_EQ(reader->read_until(
                  ioc, client, frame,
                  std::chrono::steady_clock::now()
                      + std::chrono::milliseconds{2}),
              transport_read_result::idle);
    ASSERT_TRUE(reader->pending());

    provider_io::native_socket_interrupt interrupt;
    interrupt.publish(client->next_layer().native_handle());
    bool interrupted = false;
    int interrupt_errno = 0;
    std::thread stopper([&] {
        interrupted = interrupt.request_shutdown();
        interrupt_errno = errno;
    });
    stopper.join();
    EXPECT_TRUE(interrupted) << "shutdown errno=" << interrupt_errno;

    // The real Beast composed handler is queued but cannot run until this
    // io_context is driven. Destroying the reader first proves its shared
    // state/arena remains valid for that late completion under ASAN.
    reader.reset();
    ioc->restart();
    ioc->run_for(std::chrono::milliseconds{100});
    interrupt.clear();
}

TEST(BoundedWsFrameReader, TerminalErrorDoesNotOverlapReads)
{
    auto ioc = std::make_shared<boost::asio::io_context>();
    auto websocket = std::make_shared<FakeAsyncWebSocket>(*ioc);
    provider_ws::BoundedFrameReader<FakeAsyncWebSocket> reader;
    std::string_view frame;

    EXPECT_EQ(reader.read_until(
                  ioc, websocket, frame,
                  std::chrono::steady_clock::now()
                      + std::chrono::milliseconds{1}),
              transport_read_result::idle);
    EXPECT_EQ(reader.read_until(
                  ioc, websocket, frame,
                  std::chrono::steady_clock::now()
                      + std::chrono::milliseconds{1}),
              transport_read_result::idle);
    EXPECT_EQ(websocket->start_count(), 1U);

    websocket->complete({}, boost::asio::error::connection_reset);
    EXPECT_EQ(reader.read_until(
                  ioc, websocket, frame,
                  std::chrono::steady_clock::now()
                      + std::chrono::milliseconds{100}),
              transport_read_result::terminal);
    EXPECT_EQ(reader.last_error(), boost::asio::error::connection_reset);
    EXPECT_EQ(websocket->start_count(), 1U);
}

TEST(BoundedWsFrameReader, PrewarmedRealBeastReadUsesNoHeap)
{
    namespace websocket = boost::beast::websocket;
    using local_socket = boost::asio::local::stream_protocol::socket;
    using test_websocket = websocket::stream<local_socket>;
    auto ioc = std::make_shared<boost::asio::io_context>();
    auto client = std::make_shared<test_websocket>(*ioc);
    test_websocket server(*ioc);
    boost::asio::local::connect_pair(
        client->next_layer(), server.next_layer());

    boost::beast::error_code client_ec;
    boost::beast::error_code server_ec;
    client->async_handshake("localhost", "/",
        [&](boost::beast::error_code ec) { client_ec = ec; });
    server.async_accept(
        [&](boost::beast::error_code ec) { server_ec = ec; });
    ioc->run();
    ASSERT_FALSE(client_ec);
    ASSERT_FALSE(server_ec);

    provider_ws::BoundedFrameReader<test_websocket> reader;
    constexpr std::string_view payload = R"({"stream":"trade"})";

    auto append_frame = [&]() {
        server.write(boost::asio::buffer(payload));
    };
    auto one_read = [&]() {
        std::string_view frame;
        const auto result = reader.read_until(
            ioc, client, frame,
            std::chrono::steady_clock::now() + std::chrono::seconds{1});
        return result == transport_read_result::frame && frame == payload;
    };

    for (int i = 0; i < 16; ++i)
    {
        append_frame();
        ASSERT_TRUE(one_read());
    }
    bool exact = true;
    truetest::test::alloc::snapshot allocations{};
    for (int i = 0; i < 1'000; ++i)
    {
        append_frame();
        truetest::test::alloc::measure_window window;
        exact = exact && one_read();
        const auto sample = window.total();
        allocations.count += sample.count;
        allocations.bytes += sample.bytes;
    }
    EXPECT_TRUE(exact);
    EXPECT_EQ(allocations.count, 0U);
    EXPECT_EQ(allocations.bytes, 0U);
}

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

TEST(RecordingTransport, ForwardsWrappedTerminalStatus)
{
	auto inner = std::make_shared<MockBatchTransport>(
		std::vector<std::string>{"only"});
	RecordingTransport recording(inner, "/dev/null");
	ASSERT_TRUE(recording.open());
	ASSERT_TRUE(recording.read_line().has_value());
	EXPECT_FALSE(recording.read_line().has_value());
	EXPECT_EQ(recording.terminal_status(),
	          transport_terminal_status::clean_eof);
}
