#include <gtest/gtest.h>

#ifdef HAS_GATE

#include "providers/provider_registry.h"
#include "providers/gate/gate_futures_provider.h"

#include <memory>
#include <stdexcept>
#include <string>

namespace {

std::shared_ptr<GateFuturesProvider> create(const provider_config& cfg,
                                            const char* name = "gate-futures")
{
    auto p = ProviderRegistry::instance().create(name, cfg);
    return std::dynamic_pointer_cast<GateFuturesProvider>(p);
}

} // namespace

TEST(GateFuturesRegister, IsRegistered)
{
    EXPECT_TRUE(ProviderRegistry::instance().has("gate-futures"));
    EXPECT_TRUE(ProviderRegistry::instance().has("gate"));
}

TEST(GateFuturesRegister, RequiresSymbol)
{
    provider_config cfg;
    EXPECT_THROW(
        ProviderRegistry::instance().create("gate-futures", cfg),
        std::runtime_error);
}

TEST(GateFuturesRegister, NormalizesBareSymbolToUnderscore)
{
    provider_config cfg;
    cfg["symbol"] = "BTCUSDT";

    auto p = create(cfg);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->symbol(), "BTC_USDT");
    EXPECT_EQ(p->name(), "gate-futures");
}

TEST(GateFuturesRegister, DefaultsToMainnet)
{
    provider_config cfg;
    cfg["symbol"] = "BTC_USDT";

    auto p = create(cfg);
    ASSERT_NE(p, nullptr);
    EXPECT_FALSE(p->is_testnet());
    EXPECT_EQ(p->endpoints().rest_host, "api.gateio.ws");
}

TEST(GateFuturesRegister, TestnetFlag)
{
    provider_config cfg;
    cfg["symbol"]  = "BTC_USDT";
    cfg["testnet"] = "yes";

    auto p = create(cfg);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(p->is_testnet());
}

TEST(GateFuturesRegister, AliasGateCreatesSameType)
{
    provider_config cfg;
    cfg["symbol"] = "ETH_USDT";

    auto p = create(cfg, "gate");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->name(), "gate-futures");
}

TEST(GateFuturesRegister, UserIdAndDepthStored)
{
    provider_config cfg;
    cfg["symbol"]       = "BTC_USDT";
    cfg["user_id"]      = "123456";
    cfg["depth_stream"] = "100ms:100";

    auto p = create(cfg);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->user_id(), "123456");
    EXPECT_EQ(p->depth_spec(), "100ms:100");
}

TEST(GateFuturesRegister, Phase0OpenRefuses)
{
    provider_config cfg;
    cfg["symbol"] = "BTC_USDT";

    auto p = create(cfg);
    ASSERT_NE(p, nullptr);
    EXPECT_FALSE(p->open());
    EXPECT_EQ(p->lifecycle_state(), IProvider::lifecycle::error);
    EXPECT_EQ(p->get_transport(), nullptr);
    EXPECT_EQ(p->get_execution_adapter(), nullptr);
    p->close();
    EXPECT_EQ(p->lifecycle_state(), IProvider::lifecycle::closed);
}

#endif // HAS_GATE
