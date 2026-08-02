#include <gtest/gtest.h>

#ifdef HAS_BITGET

#include "providers/provider_registry.h"
#include "providers/bitget/bitget_futures_order_encoder.h"
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

// Classic / unknown surface is Phase 4 — register must refuse loudly.
TEST(BitgetFuturesRegister, RefusesNonUtaApiSurface)
{
    provider_config cfg;
    cfg["symbol"]      = "BTCUSDT";
    cfg["api_surface"] = "classic";
    EXPECT_THROW(
        ProviderRegistry::instance().create("bitget-futures", cfg),
        std::runtime_error);
}

TEST(BitgetFuturesRegister, AcceptsUtaApiSurfaceCaseInsensitive)
{
    provider_config cfg;
    cfg["symbol"]      = "BTCUSDT";
    cfg["api_surface"] = "UTA";
    auto p = create(cfg);
    ASSERT_NE(p, nullptr);
}

// Direct set_api_surface bypass of register still refused at open().
TEST(BitgetFuturesRegister, OpenRefusesNonUtaApiSurface)
{
    provider_config cfg;
    cfg["symbol"] = "BTCUSDT";
    auto p = create(cfg);
    ASSERT_NE(p, nullptr);
    p->set_api_surface("classic");

    engine_config ec;
    ec.mode = engine_mode::backtest;
    p->configure(ec);

    EXPECT_FALSE(p->open());
    EXPECT_EQ(p->lifecycle_state(), IProvider::lifecycle::error);
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

// Live + key but missing secret/passphrase refuses before any network I/O.
TEST(BitgetFuturesRegister, LiveWithKeysMissingPassphraseRefusesOpen)
{
    provider_config cfg;
    cfg["symbol"]  = "BTCUSDT";
    cfg["api_key"] = "not-a-real-key";
    // no api_secret / api_passphrase

    auto p = create(cfg);
    ASSERT_NE(p, nullptr);

    engine_config ec;
    ec.mode = engine_mode::live;
    p->configure(ec);

    EXPECT_FALSE(p->open());
    EXPECT_EQ(p->lifecycle_state(), IProvider::lifecycle::error);
}

// Missing secret alone also refuses offline.
TEST(BitgetFuturesRegister, LiveWithKeysMissingSecretRefusesOpen)
{
    provider_config cfg;
    cfg["symbol"]         = "BTCUSDT";
    cfg["api_key"]        = "not-a-real-key";
    cfg["api_passphrase"] = "pass";
    // no api_secret

    auto p = create(cfg);
    ASSERT_NE(p, nullptr);

    engine_config ec;
    ec.mode = engine_mode::live;
    p->configure(ec);

    EXPECT_FALSE(p->open());
    EXPECT_EQ(p->lifecycle_state(), IProvider::lifecycle::error);
}

// Risk caps are applied at the top of open() before the live refuse, so
// live+keys still builds FuturesRiskCheck without touching the network.
TEST(BitgetFuturesRegister, RiskCapsInstallRiskCheckOnOpen)
{
    provider_config cfg;
    cfg["symbol"]            = "BTCUSDT";
    cfg["api_key"]           = "not-a-real-key";
    cfg["max_notional_usdt"] = "1000";

    auto p = create(cfg);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->get_risk_check(), nullptr); // not built until open()

    engine_config ec;
    ec.mode = engine_mode::live;
    p->configure(ec);

    EXPECT_FALSE(p->open()); // missing secret/passphrase after risk_check
    EXPECT_EQ(p->lifecycle_state(), IProvider::lifecycle::error);
    EXPECT_NE(p->get_risk_check(), nullptr);
}

TEST(BitgetOneWayHoldMode, AcceptsOneWay)
{
    EXPECT_TRUE(bitget::check_one_way_hold_mode(
        R"({"code":"00000","data":{"holdMode":"one_way_mode"}})").empty());
}

TEST(BitgetOneWayHoldMode, RefusesHedge)
{
    auto note = bitget::check_one_way_hold_mode(
        R"({"code":"00000","data":{"holdMode":"hedge_mode"}})");
    EXPECT_NE(note.find("hedge"), std::string::npos);
}

TEST(BitgetOneWayHoldMode, RefusesMissing)
{
    auto note = bitget::check_one_way_hold_mode(
        R"({"code":"00000","data":{}})");
    EXPECT_NE(note.find("holdMode"), std::string::npos);
}

TEST(BitgetMarginTypeStrict, MatchPasses)
{
    const char* body =
        R"({"code":"00000","data":{"holdMode":"one_way_mode",)"
        R"("symbolConfigList":[{"symbol":"BTCUSDT","marginMode":"crossed"}]}})";
    EXPECT_TRUE(bitget::check_margin_type_strict(
        body, "BTCUSDT", "CROSSED").empty());
    EXPECT_TRUE(bitget::check_margin_type_strict(
        body, "BTCUSDT", "crossed").empty());
}

TEST(BitgetMarginTypeStrict, MismatchRefuses)
{
    const char* body =
        R"({"code":"00000","data":{"symbolConfigList":[)"
        R"({"symbol":"BTCUSDT","marginMode":"isolated"}]}})";
    auto note = bitget::check_margin_type_strict(body, "BTCUSDT", "CROSSED");
    EXPECT_NE(note.find("margin-type-strict"), std::string::npos);
    EXPECT_NE(note.find("ISOLATED"), std::string::npos);
}

TEST(BitgetMarginTypeStrict, MissingSymbolRefuses)
{
    const char* body =
        R"({"code":"00000","data":{"symbolConfigList":[)"
        R"({"symbol":"ETHUSDT","marginMode":"crossed"}]}})";
    auto note = bitget::check_margin_type_strict(body, "BTCUSDT", "CROSSED");
    EXPECT_NE(note.find("not found"), std::string::npos);
}

TEST(BitgetShortClientOidMinter, FitsCharsetAndLength)
{
    bitget::ShortClientOidMinter m(/*seed=*/0xdeadbeefull, /*epoch_ms=*/1'700'000'000'000);
    for (int i = 0; i < 100; ++i)
    {
        auto id = m.next();
        EXPECT_LE(id.size(), 32u) << id;
        EXPECT_TRUE(BitgetFuturesOrderEncoder::valid_client_oid(id)) << id;
    }
}

#endif // HAS_BITGET
