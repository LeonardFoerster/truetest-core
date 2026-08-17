#include <gtest/gtest.h>

#ifdef HAS_BITUNIX

#include "providers/provider_registry.h"
#include "providers/bitunix/bitunix_futures_provider.h"
#include "engine/engine_config.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {

class FeedTransportStub final : public IDataTransport
{
public:
    bool open() override { return true; }
    void close() override {}
    bool is_open() const override { return true; }
    std::optional<std::string> read_line() override { return std::nullopt; }
};

class FeedParserStub final : public IDataParser<provider::event>
{
public:
    std::optional<provider::event> parse_record(const std::string&) override
    {
        return std::nullopt;
    }
};

class BitunixFeedProviderForTest final : public BitunixFuturesProvider
{
public:
    using BitunixFuturesProvider::BitunixFuturesProvider;

    void set_feed_handles(
        std::shared_ptr<IDataTransport> transport,
        std::shared_ptr<IDataParser<provider::event>> parser)
    {
        transport_ = std::move(transport);
        parser_ = std::move(parser);
    }

    std::shared_ptr<IDataTransport> get_transport() override
    {
        return transport_;
    }

    std::shared_ptr<IDataParser<provider::event>> get_event_parser() override
    {
        return parser_;
    }

private:
    std::shared_ptr<IDataTransport> transport_;
    std::shared_ptr<IDataParser<provider::event>> parser_;
};

} // namespace

// Registration runs via static init in bitunix_futures_register.cpp.

TEST(BitunixFuturesRegister, NamesAvailable)
{
    auto names = ProviderRegistry::instance().available();
    bool has_canonical = false;
    bool has_alias = false;
    for (const auto& n : names)
    {
        if (n == "bitunix-futures") has_canonical = true;
        if (n == "bitunix") has_alias = true;
    }
    EXPECT_TRUE(has_canonical);
    EXPECT_TRUE(has_alias);
}

TEST(BitunixFuturesRegister, CreateRequiresSymbol)
{
    provider_config cfg;
    EXPECT_THROW(
        ProviderRegistry::instance().create("bitunix-futures", cfg),
        std::runtime_error);
}

TEST(BitunixFuturesRegister, CreateWithSymbol)
{
    provider_config cfg;
    cfg["symbol"] = "BTCUSDT";
    cfg["stream"] = "trade";
    auto p = ProviderRegistry::instance().create("bitunix", cfg);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->name(), "bitunix-futures");
    EXPECT_TRUE(p->has_data_feed());
    EXPECT_TRUE(p->supports_event_stream());
    EXPECT_FALSE(p->get_market_data_feed().has_value());
}

TEST(BitunixFuturesRegister, TradeMarketDataFeedDeclaresOnlyTrades)
{
    BitunixFeedProviderForTest p("btcusdt", "trade");
    auto transport = std::make_shared<FeedTransportStub>();
    auto parser = std::make_shared<FeedParserStub>();
    p.set_feed_handles(transport, parser);

    auto feed = p.get_market_data_feed();
    ASSERT_TRUE(feed.has_value());
    EXPECT_TRUE(feed->ready());
    EXPECT_EQ(feed->transport, transport);
    EXPECT_EQ(feed->parser, parser);

    ASSERT_TRUE(feed->request.has_value());
    EXPECT_EQ(feed->request->symbol, "BTCUSDT");
    ASSERT_EQ(feed->request->channels.size(), 1U);
    EXPECT_EQ(feed->request->channels.front().kind,
              market_data_channel_kind::trades);

    ASSERT_TRUE(feed->capabilities.has_value());
    EXPECT_TRUE(feed->capabilities->trades);
    EXPECT_FALSE(feed->capabilities->candles);
    EXPECT_FALSE(feed->capabilities->l2_snapshots);
    EXPECT_FALSE(feed->capabilities->l2_deltas);
    EXPECT_EQ(feed->capabilities->max_l2_depth, 0U);
    EXPECT_TRUE(feed->capabilities->event_order_is_receive_order);
}

TEST(BitunixFuturesRegister, UnsupportedPublicStreamIsRefusedBeforeConnection)
{
    BitunixFuturesProvider p("BTCUSDT", "ticker");
    auto ep = bitunix::mainnet();
    ep.ws_public_host = "127.0.0.1";
    ep.ws_port = "1";
    p.set_endpoints(std::move(ep));

    engine_config ecfg;
    ecfg.mode = engine_mode::backtest;
    p.configure(ecfg);
    EXPECT_FALSE(p.open());
    EXPECT_EQ(p.lifecycle_state(), IProvider::lifecycle::error);
    EXPECT_EQ(p.get_transport(), nullptr);
    EXPECT_FALSE(p.get_market_data_feed().has_value());
}

TEST(BitunixFuturesRegister, LiveOpenRefused)
{
    provider_config cfg;
    cfg["symbol"] = "ETHUSDT";
    auto p = ProviderRegistry::instance().create("bitunix-futures", cfg);
    ASSERT_NE(p, nullptr);

    engine_config ecfg;
    ecfg.mode = engine_mode::live;
    p->configure(ecfg);
    EXPECT_FALSE(p->open());
    EXPECT_EQ(p->lifecycle_state(), IProvider::lifecycle::error);
}

#else

TEST(BitunixFuturesRegister, SkippedWithoutHasBitunix)
{
    GTEST_SKIP() << "HAS_BITUNIX not defined";
}

#endif // HAS_BITUNIX
