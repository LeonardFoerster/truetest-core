#include <gtest/gtest.h>

#ifdef HAS_GATE

#include "engine/engine_config.h"
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

// Phase 1: event-stream path always advertised so main.inc does not need
// a HAS_GATE branch for trade parsers.
TEST(GateFuturesRegister, Phase1EventStreamAndParser)
{
    provider_config cfg;
    cfg["symbol"] = "BTC_USDT";

    auto p = create(cfg);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(p->supports_event_stream());
    EXPECT_NE(p->get_event_parser(), nullptr);
    EXPECT_TRUE(p->has_data_feed());
    EXPECT_TRUE(p->has_execution());
}

// Live + keys: Phase 2 runs REST clock/contract probe, then still refuses
// until Phase 3–4 safety/bridge. No live orders placed.
// When public REST is reachable, instrument cache is filled before refuse.
// When REST is unreachable, open fails earlier with empty instrument cache.
TEST(GateFuturesRegister, Phase2LiveWithKeysRefusesUntilSafetyWired)
{
    provider_config cfg;
    cfg["symbol"]     = "BTC_USDT";
    cfg["api_key"]    = "test-key";
    cfg["api_secret"] = "test-secret";

    auto p = create(cfg);
    ASSERT_NE(p, nullptr);

    engine_config ec;
    ec.mode = engine_mode::live;
    p->configure(ec);

    EXPECT_FALSE(p->open());
    EXPECT_EQ(p->lifecycle_state(), IProvider::lifecycle::error);

    // Optional network path: if Gate public REST answered, Phase 2 filled
    // instrument_spec + quanto before the intentional Phase 3–4 refuse.
    if (auto inst = p->get_instrument("BTC_USDT"))
    {
        EXPECT_EQ(inst->symbol, "BTC_USDT");
        EXPECT_GT(inst->tick_size, 0.0);
        EXPECT_GT(inst->lot_size, 0.0);
        EXPECT_GT(p->quanto_multiplier(), 0.0);
    }
    else
    {
        EXPECT_DOUBLE_EQ(p->quanto_multiplier(), 0.0);
    }

    p->close();
    EXPECT_EQ(p->lifecycle_state(), IProvider::lifecycle::closed);
}

TEST(GateFuturesRegister, GetInstrumentEmptyBeforeOpen)
{
    provider_config cfg;
    cfg["symbol"] = "BTC_USDT";
    auto p = create(cfg);
    ASSERT_NE(p, nullptr);
    EXPECT_FALSE(p->get_instrument("BTC_USDT").has_value());
}

// Live without secret refuses before network (fail-closed).
TEST(GateFuturesRegister, Phase2LiveMissingSecretRefuses)
{
    provider_config cfg;
    cfg["symbol"]  = "BTC_USDT";
    cfg["api_key"] = "test-key";
    // no api_secret

    auto p = create(cfg);
    ASSERT_NE(p, nullptr);

    engine_config ec;
    ec.mode = engine_mode::live;
    p->configure(ec);

    EXPECT_FALSE(p->open());
    EXPECT_EQ(p->lifecycle_state(), IProvider::lifecycle::error);
    EXPECT_FALSE(p->get_instrument("BTC_USDT").has_value());
    p->close();
}

#endif // HAS_GATE
