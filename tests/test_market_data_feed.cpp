#include <gtest/gtest.h>

#include "providers/market_data_feed.h"
#include "providers/provider.h"
#include "providers/binance/binance_recorder.h"

namespace {

class StubTransport final : public IDataTransport
{
public:
    bool open() override { return true; }
    void close() override {}
    bool is_open() const override { return true; }
    std::optional<std::string> read_line() override { return std::nullopt; }
};

class StubEventParser final : public IDataParser<provider::event>
{
public:
    std::optional<provider::event> parse_record(const std::string&) override
    {
        return std::nullopt;
    }
};

class BoundedStubTransport final : public IDataTransport
{
public:
    bool open() override { return true; }
    void close() override {}
    bool is_open() const override { return true; }
    std::optional<std::string> read_line() override { return std::nullopt; }
    bool supports_bounded_idle_read() const override { return true; }
    transport_read_result read_frame_until(
        std::string_view& out,
        std::chrono::steady_clock::time_point) override
    {
        out = frame_;
        return transport_read_result::frame;
    }

private:
    static constexpr std::string_view frame_ = "bounded-frame";
};

class EventStreamProviderStub final : public IProvider
{
public:
    explicit EventStreamProviderStub(bool with_transport, bool with_parser)
    {
        if (with_transport)
            transport_ = std::make_shared<StubTransport>();
        if (with_parser)
            parser_ = std::make_shared<StubEventParser>();
    }

    std::string name() const override { return "event-stream-stub"; }
    bool has_data_feed() const override { return true; }
    bool has_execution() const override { return false; }
    bool open() override { return true; }
    void close() override {}
    std::shared_ptr<IDataTransport> get_transport() override
    {
        ++transport_calls;
        return transport_;
    }
    std::shared_ptr<IExecutionAdapter> get_execution_adapter() override { return nullptr; }
    bool supports_event_stream() const override { return true; }
    std::shared_ptr<IDataParser<provider::event>> get_event_parser() override
    {
        ++parser_calls;
        return parser_;
    }

    std::shared_ptr<IDataTransport> transport_handle() const { return transport_; }
    std::shared_ptr<IDataParser<provider::event>> parser_handle() const { return parser_; }

    int transport_calls = 0;
    int parser_calls = 0;

private:
    std::shared_ptr<IDataTransport> transport_;
    std::shared_ptr<IDataParser<provider::event>> parser_;
};

} // namespace

TEST(MarketDataFeed, RequiresMatchedTransportAndParser)
{
    MarketDataFeed feed;
    EXPECT_FALSE(feed.ready());

    feed.transport = std::make_shared<StubTransport>();
    EXPECT_FALSE(feed.ready());

    feed.parser = std::make_shared<StubEventParser>();
    EXPECT_TRUE(feed.ready());
}

TEST(MarketDataFeed, KeepsSemanticRequestAndExplicitL2Capabilities)
{
    MarketDataFeed feed;
    feed.request = market_data_request{
        .symbol = "BTCUSDT",
        .channels = {
        {market_data_channel_kind::trades},
        {market_data_channel_kind::l2_snapshot, 20},
        },
    };
    feed.capabilities = market_data_capabilities{
        .trades = true,
        .l2_snapshots = true,
        .max_l2_depth = 20,
    };

    ASSERT_TRUE(feed.request.has_value());
    ASSERT_TRUE(feed.capabilities.has_value());
    ASSERT_EQ(feed.request->channels.size(), 2U);
    EXPECT_EQ(feed.request->channels[1].kind,
              market_data_channel_kind::l2_snapshot);
    EXPECT_EQ(feed.request->channels[1].depth, 20U);
    EXPECT_FALSE(feed.capabilities->l2_deltas);
}

TEST(MarketDataFeed, ProviderBundlesOnlyACompleteExistingEventStream)
{
    EventStreamProviderStub missing_transport(/*with_transport=*/false,
                                              /*with_parser=*/true);
    EXPECT_FALSE(missing_transport.get_market_data_feed().has_value());

    EventStreamProviderStub missing_parser(/*with_transport=*/true,
                                           /*with_parser=*/false);
    EXPECT_FALSE(missing_parser.get_market_data_feed().has_value());

    EventStreamProviderStub complete(/*with_transport=*/true,
                                     /*with_parser=*/true);
    auto feed = complete.get_market_data_feed();
    ASSERT_TRUE(feed.has_value());
    EXPECT_TRUE(feed->ready());
    EXPECT_EQ(feed->transport, complete.transport_handle());
    EXPECT_EQ(feed->parser, complete.parser_handle());
    EXPECT_EQ(complete.transport_calls, 1);
    EXPECT_EQ(complete.parser_calls, 1);
    EXPECT_FALSE(feed->request.has_value());
    EXPECT_FALSE(feed->capabilities.has_value());
}

TEST(MarketDataFeed, RecordingDecoratorPreservesBoundedReads)
{
    auto inner = std::make_shared<BoundedStubTransport>();
    RecordingTransport recording(inner, "/dev/null");

    EXPECT_TRUE(recording.supports_bounded_idle_read());
    std::string_view frame;
    EXPECT_EQ(recording.read_frame_until(
                  frame, std::chrono::steady_clock::now()),
              transport_read_result::frame);
    EXPECT_EQ(frame, "bounded-frame");
}
