#include <gtest/gtest.h>

#ifdef HAS_BITUNIX

#include "providers/provider_registry.h"
#include "providers/bitunix/bitunix_futures_provider.h"
#include "engine/engine_config.h"

#include <memory>

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
