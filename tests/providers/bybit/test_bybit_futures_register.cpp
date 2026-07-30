#include <gtest/gtest.h>

#ifdef HAS_BYBIT

#include "engine/engine_config.h"
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

TEST(BybitFuturesRegister, LiveOpenStillRefuses)
{
    // Phase 1: live with keys refuses (Phase 2+).
    // Shadow/paper open needs network — not exercised offline.
    provider_config cfg;
    cfg["symbol"] = "BTCUSDT";

    auto p = create(cfg);
    ASSERT_NE(p, nullptr);

    engine_config ec;
    ec.mode = engine_mode::live;
    p->configure(ec);

    // Inject keys via factory-style: re-create with api_key in pcfg.
    provider_config live_cfg;
    live_cfg["symbol"] = "BTCUSDT";
    live_cfg["api_key"] = "test_key";
    live_cfg["api_secret"] = "test_secret";
    auto live = create(live_cfg);
    ASSERT_NE(live, nullptr);
    live->configure(ec);
    EXPECT_FALSE(live->open());
    EXPECT_EQ(live->lifecycle_state(), IProvider::lifecycle::error);
    live->close();
}

TEST(BybitFuturesRegister, AlwaysSupportsEventStream)
{
    // Trade-only and depth modes both advertise event stream so main.inc
    // never falls through to CsvTickParser for bybit-futures.
    provider_config cfg;
    cfg["symbol"] = "BTCUSDT";

    auto p = create(cfg);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(p->supports_event_stream());
    ASSERT_NE(p->get_event_parser(), nullptr);

    cfg["depth_stream"] = "orderbook.50";
    auto with_depth = create(cfg);
    ASSERT_NE(with_depth, nullptr);
    EXPECT_TRUE(with_depth->supports_event_stream());
    ASSERT_NE(with_depth->get_event_parser(), nullptr);
}

#endif // HAS_BYBIT
