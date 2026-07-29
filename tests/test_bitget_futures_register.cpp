#include <gtest/gtest.h>

#ifdef HAS_BITGET

#include "providers/provider_registry.h"
#include "providers/bitget/bitget_futures_provider.h"

#include <memory>
#include <stdexcept>
#include <string>

namespace {

std::shared_ptr<BitgetFuturesProvider> create(const provider_config& cfg,
                                              const char* name = "bitget-futures")
{
    auto p = ProviderRegistry::instance().create(name, cfg);
    return std::dynamic_pointer_cast<BitgetFuturesProvider>(p);
}

} // namespace

TEST(BitgetFuturesRegister, IsRegistered)
{
    EXPECT_TRUE(ProviderRegistry::instance().has("bitget-futures"));
    EXPECT_TRUE(ProviderRegistry::instance().has("bitget"));
}

TEST(BitgetFuturesRegister, RequiresSymbol)
{
    provider_config cfg;
    EXPECT_THROW(
        ProviderRegistry::instance().create("bitget-futures", cfg),
        std::runtime_error);
}

TEST(BitgetFuturesRegister, DefaultsToMainnetEndpoints)
{
    provider_config cfg;
    cfg["symbol"] = "BTCUSDT";

    auto p = create(cfg);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->name(), "bitget-futures");
    EXPECT_FALSE(p->is_demo());
}

TEST(BitgetFuturesRegister, DemoFlagSelectsDemoEndpoints)
{
    provider_config cfg;
    cfg["symbol"] = "BTCUSDT";
    cfg["demo"]   = "1";

    auto p = create(cfg);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(p->is_demo());
}

TEST(BitgetFuturesRegister, TestnetFlagSelectsDemoEndpoints)
{
    provider_config cfg;
    cfg["symbol"]  = "BTCUSDT";
    cfg["testnet"] = "true";

    auto p = create(cfg);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(p->is_demo());
}

TEST(BitgetFuturesRegister, PaptradingFlagSelectsDemoEndpoints)
{
    provider_config cfg;
    cfg["symbol"]     = "BTCUSDT";
    cfg["paptrading"] = "1";

    auto p = create(cfg);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(p->is_demo());
}

TEST(BitgetFuturesRegister, AliasBitgetCreatesSameType)
{
    provider_config cfg;
    cfg["symbol"] = "BTCUSDT";

    auto p = create(cfg, "bitget");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->name(), "bitget-futures");
    EXPECT_FALSE(p->is_demo());
}

TEST(BitgetFuturesRegister, DepthStreamEnablesEventStream)
{
    provider_config cfg;
    cfg["symbol"]       = "BTCUSDT";
    cfg["depth_stream"] = "books5";

    auto p = create(cfg);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(p->supports_event_stream());
    EXPECT_NE(p->get_event_parser(), nullptr);
}

TEST(BitgetFuturesRegister, TradeOnlyHasNoEventStream)
{
    provider_config cfg;
    cfg["symbol"] = "BTCUSDT";
    cfg["stream"] = "trade";

    auto p = create(cfg);
    ASSERT_NE(p, nullptr);
    EXPECT_FALSE(p->supports_event_stream());
    EXPECT_EQ(p->get_event_parser(), nullptr);
}

TEST(BitgetFuturesRegister, PassphraseStored)
{
    provider_config cfg;
    cfg["symbol"]         = "BTCUSDT";
    cfg["api_passphrase"] = "secret-pass";

    auto p = create(cfg);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->api_passphrase(), "secret-pass");
}

TEST(BitgetFuturesRegister, HasDataAndExecution)
{
    provider_config cfg;
    cfg["symbol"] = "BTCUSDT";

    auto p = create(cfg);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(p->has_data_feed());
    EXPECT_TRUE(p->has_execution());
}

#endif // HAS_BITGET
