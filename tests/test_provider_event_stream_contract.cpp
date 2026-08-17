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

TEST(ProviderEventStreamContract, BinanceDirectLiveOpenRequiresCompleteCredentials)
{
    BinanceProvider p("btcusdt", "trade");
    engine_config cfg;
    cfg.mode = engine_mode::live;
    p.configure(cfg);

    EXPECT_FALSE(p.open());
    EXPECT_EQ(p.lifecycle_state(), IProvider::lifecycle::error);
}
#endif
