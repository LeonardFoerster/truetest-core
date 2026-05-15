#include <gtest/gtest.h>

#ifdef HAS_BINANCE

#include "providers/provider_registry.h"
#include "providers/binance/binance_futures_provider.h"

#include <memory>
#include <stdexcept>
#include <string>

namespace {

std::shared_ptr<BinanceFuturesProvider> create(const provider_config& cfg)
{
    auto p = ProviderRegistry::instance().create("binance-futures", cfg);
    return std::dynamic_pointer_cast<BinanceFuturesProvider>(p);
}

}

TEST(BinanceFuturesRegister, IsRegistered)
{
    EXPECT_TRUE(ProviderRegistry::instance().has("binance-futures"));
}

TEST(BinanceFuturesRegister, RequiresSymbol)
{
    provider_config cfg;
    EXPECT_THROW(
        ProviderRegistry::instance().create("binance-futures", cfg),
        std::runtime_error);
}

TEST(BinanceFuturesRegister, DefaultsToMainnetEndpoints)
{
    provider_config cfg;
    cfg["symbol"] = "btcusdt";

    auto p = create(cfg);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->name(), "binance-futures");
    EXPECT_FALSE(p->is_testnet());
}

TEST(BinanceFuturesRegister, TestnetFlagSelectsTestnetEndpoints)
{
    provider_config cfg;
    cfg["symbol"]  = "btcusdt";
    cfg["testnet"] = "1";

    auto p = create(cfg);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(p->is_testnet());
}

// `stream.binancefuture.com` carries no "testnet" substring but is the
// futures testnet WS host — looks_like_testnet_host() must catch it on
// the `binancefuture` token alone.
TEST(BinanceFuturesRegister, BinancefutureHostInferredAsTestnet)
{
    provider_config cfg;
    cfg["symbol"] = "btcusdt";
    cfg["host"]   = "stream.binancefuture.com";

    auto p = create(cfg);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(p->is_testnet());
}

TEST(BinanceFuturesRegister, MainnetHostStaysMainnet)
{
    provider_config cfg;
    cfg["symbol"] = "btcusdt";
    cfg["host"]   = "fstream.binance.com";

    auto p = create(cfg);
    ASSERT_NE(p, nullptr);
    EXPECT_FALSE(p->is_testnet());
}

#endif // HAS_BINANCE
