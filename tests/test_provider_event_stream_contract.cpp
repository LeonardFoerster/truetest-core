// Pins the IProvider unified-event-stream contract. A venue that emits native
// JSON owns its matching parser even when the selected channel has no depth.
// Providers that inherit the base defaults remain on an explicitly supported
// legacy path until they publish a complete MarketDataFeed.

#include <gtest/gtest.h>

#include "providers/provider.h"

#ifdef HAS_BINANCE
#include "providers/binance/binance_provider.h"
#endif

TEST(ProviderEventStreamContract, BaseDefaultsAreOff)
{
    // A hand-rolled stub that inherits defaults. The whole point of the
    // default false/nullptr is that existing providers keep working
    // without touching the new methods.
    class Stub : public IProvider
    {
    public:
        std::string name() const override { return "stub"; }
        bool has_data_feed() const override { return false; }
        bool has_execution() const override { return false; }
        bool open() override { return true; }
        void close() override {}
        std::shared_ptr<IDataTransport> get_transport() override { return nullptr; }
        std::shared_ptr<IExecutionAdapter> get_execution_adapter() override { return nullptr; }
    };

    Stub s;
    EXPECT_FALSE(s.supports_event_stream());
    EXPECT_EQ(s.get_event_parser(), nullptr);
}

TEST(ProviderEventStreamContract,
     C07_StreamingWithoutDeclaredParserCapabilityFailsClosed)
{
    std::optional<MarketDataFeed> no_feed;
    EXPECT_EQ(select_market_data_route(false, no_feed, false),
              market_data_route::legacy_non_streaming);
    EXPECT_EQ(select_market_data_route(false, no_feed, true),
              market_data_route::invalid);
    EXPECT_EQ(select_market_data_route(true, no_feed, true),
              market_data_route::invalid);

    MarketDataFeed incomplete;
    std::optional<MarketDataFeed> incomplete_feed{incomplete};
    EXPECT_EQ(select_market_data_route(true, incomplete_feed, true),
              market_data_route::invalid);
}

#ifdef HAS_BINANCE
TEST(ProviderEventStreamContract, C07_BinanceTradeOnlyDeclaresJsonParserWithoutDepth)
{
    BinanceProvider p("btcusdt", "trade");
    EXPECT_TRUE(p.supports_event_stream());
    auto parser = p.get_event_parser();
    ASSERT_NE(parser, nullptr);

    const std::string frame =
        R"({"e":"trade","E":1704067200000,"s":"BTCUSDT",)"
        R"("t":12345,"p":"42000.50","q":"0.010","b":88,)"
        R"("a":50,"T":1704067200001,"m":false})";
    auto event = parser->parse_record(frame);
    ASSERT_TRUE(event.has_value());
    ASSERT_TRUE(std::holds_alternative<provider::tick>(*event));
    const auto& tick = std::get<provider::tick>(*event);
    EXPECT_EQ(tick.symbol, "BTCUSDT");
    EXPECT_DOUBLE_EQ(tick.price, 42000.5);
    EXPECT_EQ(tick.quantity, 1'000'000);
    EXPECT_EQ(tick.native_trade_id, 12345U);
}

TEST(ProviderEventStreamContract, BinancePartialDepthWithoutTimeFailsClosed)
{
    BinanceProvider p("btcusdt", "trade");
    p.set_depth_stream("depth20@100ms");
    EXPECT_TRUE(p.supports_event_stream());
    auto parser = p.get_event_parser();
    ASSERT_NE(parser, nullptr);

    // The parser has no typed receive timestamp. The no-E snapshot must not
    // acquire invented chronology. The frozen provider capability flag needs
    // a later CCB-approved correction.
    const std::string frame =
        R"({"stream":"btcusdt@depth20@100ms","data":)"
        R"({"lastUpdateId":1,"bids":[["42000","1"]],"asks":[["42001","1"]]}})";
    auto ev = parser->parse_record(frame);
    EXPECT_FALSE(ev.has_value());
}

TEST(ProviderEventStreamContract, BinanceDirectLiveOpenRequiresCompleteCredentials)
{
    BinanceProvider p("btcusdt", "trade");
    engine_config cfg;
    cfg.mode = engine_mode::live;
    p.configure(cfg);

    EXPECT_FALSE(p.open());
    EXPECT_EQ(p.lifecycle_state(), IProvider::lifecycle::error);
}

TEST(ProviderEventStreamContract, BinanceLiveSafetyPreparationIsOperationalWithoutOpen)
{
    BinanceProvider p("btcusdt", "trade", "test-key", "test-secret");
    engine_config cfg;
    cfg.mode = engine_mode::live;
    p.configure(cfg);

    EXPECT_EQ(p.private_execution_capability_level(),
              private_execution_capability::exchange_writes);
    ASSERT_TRUE(p.prepare_write_safety());
    ASSERT_NE(p.get_reconciler(), nullptr);
    ASSERT_NE(p.get_kill_switch(), nullptr);
    EXPECT_TRUE(p.get_reconciler()->is_operational());
    EXPECT_TRUE(p.get_kill_switch()->is_operational());
    EXPECT_EQ(p.lifecycle_state(), IProvider::lifecycle::closed);
}

TEST(ProviderEventStreamContract, BinanceDirectLiveOpenCannotBypassSafetySession)
{
    BinanceProvider p("btcusdt", "trade", "test-key", "test-secret");
    engine_config cfg;
    cfg.mode = engine_mode::live;
    p.configure(cfg);

    EXPECT_FALSE(p.open());
    EXPECT_EQ(p.lifecycle_state(), IProvider::lifecycle::error);
}
#endif
