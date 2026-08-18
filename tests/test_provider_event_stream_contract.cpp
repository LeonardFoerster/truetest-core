// Pins the IProvider unified-event-stream contract:
//  - default (no depth opt-in)      -> supports_event_stream()=false, parser=null
//  - after set_depth_stream(...)    -> supports_event_stream()=true,  parser!=null
//  - local provider (no override)   -> stays on specialized bridge path

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

#ifdef HAS_BINANCE
TEST(ProviderEventStreamContract, BinanceWithoutDepthStreamIsOff)
{
    BinanceProvider p("btcusdt", "trade");
    EXPECT_FALSE(p.supports_event_stream());
    EXPECT_EQ(p.get_event_parser(), nullptr);
}

TEST(ProviderEventStreamContract, BinanceWithDepthStreamIsOn)
{
    BinanceProvider p("btcusdt", "trade");
    p.set_depth_stream("depth20@100ms");
    EXPECT_TRUE(p.supports_event_stream());
    auto parser = p.get_event_parser();
    ASSERT_NE(parser, nullptr);

    // And the parser actually dispatches on a partial-book frame.
    const std::string frame =
        R"({"stream":"btcusdt@depth20@100ms","data":)"
        R"({"lastUpdateId":1,"bids":[["42000","1"]],"asks":[["42001","1"]]}})";
    auto ev = parser->parse_record(frame);
    ASSERT_TRUE(ev.has_value());
    EXPECT_TRUE(std::holds_alternative<provider::l2_snapshot>(*ev));
}

TEST(ProviderEventStreamContract,
     BinanceLiveParserRefusesInjectedRawDiffDepthButAcceptsPartialBook)
{
    BinanceProvider p("btcusdt", "trade");
    p.set_depth_stream("depth20@100ms");
    engine_config cfg;
    cfg.mode = engine_mode::live;
    p.configure(cfg);

    auto parser = p.get_event_parser();
    ASSERT_NE(parser, nullptr);

    // Defense in depth: the declared partial-book subscription is safe, but
    // an unexpected raw delta frame must still not overwrite the live book.
    const std::string raw_diff =
        R"({"e":"depthUpdate","E":1704067200000,"s":"BTCUSDT",)"
        R"("U":1,"u":10,"b":[["42000","1"]],"a":[["42001","1"]]})";
    EXPECT_FALSE(parser->parse_record(raw_diff).has_value());
    EXPECT_EQ(parser->classify_empty_frame(raw_diff),
              empty_parse_status::malformed);

    const std::string partial_frame =
        R"({"stream":"btcusdt@depth20@100ms","data":)"
        R"({"lastUpdateId":1,"bids":[["42000","1"]],"asks":[["42001","1"]]}})";
    auto partial = parser->parse_record(partial_frame);
    ASSERT_TRUE(partial.has_value());
    EXPECT_TRUE(std::holds_alternative<provider::l2_snapshot>(*partial));
}

TEST(ProviderEventStreamContract,
     BinanceLiveOpenRefusesRawDiffDepthBeforeTransportReadiness)
{
    // Deliberately nonempty dummy credentials: if the contract check moved
    // behind the network/REST startup path this test would attempt I/O.
    BinanceProvider p("btcusdt", "trade", "key", "secret");
    p.set_depth_stream("depth@100ms");
    engine_config cfg;
    cfg.mode = engine_mode::live;
    p.configure(cfg);

    EXPECT_FALSE(p.open());
    EXPECT_EQ(p.lifecycle_state(), IProvider::lifecycle::error);
    EXPECT_EQ(p.get_transport(), nullptr);
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

TEST(ProviderEventStreamContract,
     BinanceLiveOpenRefusesUntilUnifiedIngressAndTypedBracketsAreWired)
{
    // Complete dummy credentials make this a no-I/O regression: the
    // structural live-admission guard must run before either transport or REST
    // setup while native bracket lifecycle records are still unavailable.
    BinanceProvider p("btcusdt", "trade", "key", "secret");
    engine_config cfg;
    cfg.mode = engine_mode::live;
    p.configure(cfg);

    EXPECT_FALSE(p.open());
    EXPECT_EQ(p.lifecycle_state(), IProvider::lifecycle::error);
    EXPECT_EQ(p.get_transport(), nullptr);
    EXPECT_FALSE(p.private_execution_producer_joined());
}
#endif
