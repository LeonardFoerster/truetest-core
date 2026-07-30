#include <gtest/gtest.h>

#ifdef HAS_BYBIT

#include "providers/provider_registry.h"
#include "providers/bybit/bybit_futures_provider.h"

#include <memory>
#include <stdexcept>
#include <string>

namespace {

std::shared_ptr<BybitFuturesProvider> create(const provider_config& cfg,
                                             const char* name = "bybit-futures")
{
    auto p = ProviderRegistry::instance().create(name, cfg);
    return std::dynamic_pointer_cast<BybitFuturesProvider>(p);
}

} // namespace

TEST(BybitFuturesRegister, IsRegistered)
{
    EXPECT_TRUE(ProviderRegistry::instance().has("bybit-futures"));
    EXPECT_TRUE(ProviderRegistry::instance().has("bybit"));
}

TEST(BybitFuturesRegister, RequiresSymbol)
{
    provider_config cfg;
    EXPECT_THROW(
        ProviderRegistry::instance().create("bybit-futures", cfg),
        std::runtime_error);
}

TEST(BybitFuturesRegister, DefaultsToMainnet)
{
    provider_config cfg;
    cfg["symbol"] = "BTCUSDT";

    auto p = create(cfg);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->name(), "bybit-futures");
    EXPECT_FALSE(p->is_testnet());
    EXPECT_FALSE(p->is_demo());
    EXPECT_EQ(p->endpoints().rest_host, "api.bybit.com");
}

TEST(BybitFuturesRegister, TestnetFlag)
{
    provider_config cfg;
    cfg["symbol"]  = "BTCUSDT";
    cfg["testnet"] = "1";

    auto p = create(cfg);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(p->is_testnet());
    EXPECT_FALSE(p->is_demo());
}

TEST(BybitFuturesRegister, DemoTakesPrecedenceOverTestnet)
{
    provider_config cfg;
    cfg["symbol"]  = "BTCUSDT";
    cfg["demo"]    = "true";
    cfg["testnet"] = "1";

    auto p = create(cfg);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(p->is_demo());
    EXPECT_FALSE(p->is_testnet());
}

TEST(BybitFuturesRegister, AliasBybitCreatesSameType)
{
    provider_config cfg;
    cfg["symbol"] = "ETHUSDT";

    auto p = create(cfg, "bybit");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->name(), "bybit-futures");
    EXPECT_EQ(p->symbol(), "ETHUSDT");
}

TEST(BybitFuturesRegister, DepthStreamAndRiskCaps)
{
    provider_config cfg;
    cfg["symbol"]              = "BTCUSDT";
    cfg["depth_stream"]        = "orderbook.50";
    cfg["max_notional_usdt"]   = "100";
    cfg["max_leverage"]        = "5";

    auto p = create(cfg);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->depth_stream(), "orderbook.50");
}

TEST(BybitFuturesRegister, Phase0OpenRefuses)
{
    provider_config cfg;
    cfg["symbol"] = "BTCUSDT";

    auto p = create(cfg);
    ASSERT_NE(p, nullptr);
    EXPECT_FALSE(p->open());
    EXPECT_EQ(p->lifecycle_state(), IProvider::lifecycle::error);
    EXPECT_EQ(p->get_transport(), nullptr);
    EXPECT_EQ(p->get_execution_adapter(), nullptr);
    p->close();
    EXPECT_EQ(p->lifecycle_state(), IProvider::lifecycle::closed);
}

#endif // HAS_BYBIT
