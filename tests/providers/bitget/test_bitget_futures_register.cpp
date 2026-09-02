#include <gtest/gtest.h>

#ifdef HAS_BITGET

#include "providers/provider_registry.h"
#include "providers/bitget/bitget_futures_order_encoder.h"
#include "providers/bitget/bitget_futures_provider.h"
#include "engine/live_safety_session.h"
#include "helpers/alloc_counter.h"

#include <chrono>
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

TEST(BitgetFuturesRegister, FundingParserPublishesThroughProviderIngress)
{
    BitgetFuturesUserDataParser parser;
    ProviderFundingIngress ingress;
    constexpr std::string_view frame =
        R"({"arg":{"instType":"UTA","topic":"account"},"data":[{"coin":"usdt","balance":"99.75","balanceChange":"-0.25","bizType":"funding_fee"}],"ts":1700000000000})";
    bool exact = true;
    truetest::test::alloc::snapshot allocations;
    {
        truetest::test::alloc::measure_window window;
        for (int i = 0; i < 1'000; ++i)
        {
            parsed_funding_update parsed;
            exact = exact && parser.parse_funding_update(frame, parsed)
                == funding_parse_result::valid;
            exact = exact && ingress.try_publish(
                std::chrono::system_clock::time_point(
                    std::chrono::milliseconds(parsed.event_time_ms)),
                "BTCUSDT", parsed.cash_delta);
            provider_funding_update popped;
            exact = exact && ingress.try_pop(popped)
                && popped.cash_delta == -0.25;
        }
        allocations = window.total();
    }
    EXPECT_TRUE(exact);
    EXPECT_EQ(allocations.count, 0U);
    EXPECT_EQ(allocations.bytes, 0U);

    parsed_funding_update parsed;
    ASSERT_EQ(parser.parse_funding_update(frame, parsed),
              funding_parse_result::valid);
    ASSERT_TRUE(ingress.try_publish(
        std::chrono::system_clock::time_point(
            std::chrono::milliseconds(parsed.event_time_ms)),
        "BTCUSDT", parsed.cash_delta));
    provider_funding_update update;
    ASSERT_TRUE(ingress.try_pop(update));
    EXPECT_EQ(update.symbol_view(), "BTCUSDT");
    EXPECT_DOUBLE_EQ(update.cash_delta, -0.25);
    EXPECT_FALSE(ingress.try_pop(update));
}

TEST(BitgetFuturesRegister, FundingParserSeparatesRowsAndRefusesAmbiguity)
{
    BitgetFuturesUserDataParser parser;
    parsed_funding_update parsed;

    for (const auto body : {
        R"({"arg":{"instType":"UTA","topic":"account"},"data":[{"coin":"USDT","balanceChange":"7","bizType":"transfer"},{"coin":"USDT","balanceChange":"-0.25","bizType":"funding_fee"}],"ts":1700000000000})",
        R"({"arg":{"topic":"account"},"data":[{"coin":"USDT","balanceChange":"1","bizType":"refund"}],"ts":1700000000000})",
        R"({"arg":{"topic":"account"},"data":[{"coin":"USDT","balanceChange":"1","memo":"funding_fee"}],"ts":1700000000000})",
        R"({"arg":{"topic":"account"},"data":[{"coin":"USDT","balanceChange":"1junk","bizType":"funding_fee"}],"ts":1700000000000})",
        R"({"arg":{"topic":"account"},"data":[{"coin":"USDT","balanceChange":"0","bizType":"funding_fee"}],"ts":1700000000000})",
        R"({"arg":{"topic":"account"},"data":[{"coin":"BTC","balanceChange":"1","bizType":"funding_fee"}],"ts":1700000000000})",
        R"({"arg":{"topic":"account"},"data":[{"coin":"USDT","balanceChange":"1","bizType":"funding_fee","type":"funding_fee"}],"ts":1700000000000})",
        R"({"arg":{"topic":"account"},"data":[{"coin":"USDT","balanceChange":"1","bizType":"funding_fee"}],"ts":1700000000000,"ts":1700000000001})",
        R"({"arg":{"topic":"account"},"data":[{"coin":"USDT","balanceChange":"1","bizType":"funding_fee"},{"coin":"USDT","balanceChange":"2","bizType":"funding_fee"}],"ts":1700000000000})"})
        EXPECT_EQ(parser.parse_funding_update(body, parsed),
                  funding_parse_result::invalid);

    EXPECT_EQ(parser.parse_funding_update(
        R"({"arg":{"topic":"account"},"data":[{"coin":"USDT","balanceChange":"7","bizType":"transfer"}],"ts":1700000000000})",
        parsed), funding_parse_result::not_funding);
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

TEST(BitgetFuturesRegister, C07_TradeOnlyDeclaresAndParsesEventStreamWithoutDepth)
{
    provider_config cfg;
    cfg["symbol"] = "BTCUSDT";
    cfg["stream"] = "trade";

    auto p = create(cfg);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(p->supports_event_stream());
    auto parser = p->get_event_parser();
    ASSERT_NE(parser, nullptr);

    const std::string frame = R"({
      "arg":{"instType":"usdt-futures","topic":"publicTrade","symbol":"BTCUSDT"},
      "data":[
        {"i":"1","p":"42000.5","v":"0.01","S":"buy","T":"1704067200000"},
        {"i":"2","p":"42001.0","v":"0.02","S":"sell","T":"1704067200001"}
      ],
      "ts":1704067200002
    })";
    const auto events = parser->parse_records(frame);
    ASSERT_EQ(events.size(), 2U);
    for (const auto& event : events)
    {
        ASSERT_TRUE(std::holds_alternative<provider::tick>(event));
        EXPECT_EQ(std::get<provider::tick>(event).symbol, "BTCUSDT");
    }
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

TEST(BitgetFuturesRegister, DirectLiveOpenRefusesEveryIncompleteCredentialTuple)
{
    for (unsigned mask = 0; mask < 7; ++mask)
    {
        provider_config cfg;
        cfg["symbol"] = "BTCUSDT";
        if (mask & 1u) cfg["api_key"] = "key";
        if (mask & 2u) cfg["api_secret"] = "secret";
        if (mask & 4u) cfg["api_passphrase"] = "passphrase";
        auto p = create(cfg);
        ASSERT_NE(p, nullptr);
        engine_config ec;
        ec.mode = engine_mode::live;
        p->configure(ec);
        EXPECT_FALSE(p->open()) << "mask=" << mask;
        EXPECT_EQ(p->lifecycle_state(), IProvider::lifecycle::error);
    }
}

TEST(BitgetFuturesRegister, DemoLiveSafetyPreparationIsOperationalWithoutOpen)
{
    BitgetFuturesProvider p(
        "BTCUSDT", "trade", "test-key", "test-secret", "test-passphrase");
    p.set_endpoints(bitget::uta_demo());
    engine_config cfg;
    cfg.mode = engine_mode::live;
    p.configure(cfg);

    EXPECT_TRUE(p.is_demo());
    EXPECT_EQ(p.private_execution_capability_level(),
              private_execution_capability::exchange_writes);
    ASSERT_TRUE(p.prepare_write_safety());
    ASSERT_NE(p.get_reconciler(), nullptr);
    ASSERT_NE(p.get_kill_switch(), nullptr);
    EXPECT_TRUE(p.get_reconciler()->is_operational());
    EXPECT_TRUE(p.get_kill_switch()->is_operational());
    EXPECT_EQ(p.lifecycle_state(), IProvider::lifecycle::closed);
}

TEST(BitgetFuturesRegister, DemoWriteStartupRejectsNoopReconcilerBeforeOpen)
{
    auto p = std::make_shared<BitgetFuturesProvider>(
        "BTCUSDT", "trade", "test-key", "test-secret", "test-passphrase");
    p->set_endpoints(bitget::uta_demo());
    engine_config cfg;
    cfg.mode = engine_mode::live;
    p->configure(cfg);
    live_safety_requirements requirements;
    requirements.target_allows_private_exchange_writes = true;
    requirements.private_exchange_execution_requested = true;
    requirements.reconciler = std::make_shared<NoopReconciler>();
    LiveSafetySession session(
        p, std::move(requirements), std::chrono::milliseconds{50});

    EXPECT_FALSE(session.open_provider());
    EXPECT_EQ(p->lifecycle_state(), IProvider::lifecycle::closed);
    EXPECT_NE(session.startup_error().find("operational reconciler"),
              std::string::npos);
}

TEST(BitgetFuturesRegister, DemoWriteStartupRejectsNoopKillSwitchBeforeOpen)
{
    auto p = std::make_shared<BitgetFuturesProvider>(
        "BTCUSDT", "trade", "test-key", "test-secret", "test-passphrase");
    p->set_endpoints(bitget::uta_demo());
    engine_config cfg;
    cfg.mode = engine_mode::live;
    p->configure(cfg);
    live_safety_requirements requirements;
    requirements.target_allows_private_exchange_writes = true;
    requirements.private_exchange_execution_requested = true;
    requirements.kill_switch = std::make_shared<NoopKillSwitch>();
    LiveSafetySession session(
        p, std::move(requirements), std::chrono::milliseconds{50});

    EXPECT_FALSE(session.open_provider());
    EXPECT_EQ(p->lifecycle_state(), IProvider::lifecycle::closed);
    EXPECT_NE(session.startup_error().find("operational kill switch"),
              std::string::npos);
}

TEST(BitgetFuturesRegister, DirectLiveOpenCannotBypassSafetySession)
{
    BitgetFuturesProvider p(
        "BTCUSDT", "trade", "test-key", "test-secret", "test-passphrase");
    engine_config cfg;
    cfg.mode = engine_mode::live;
    p.configure(cfg);

    EXPECT_FALSE(p.open());
    EXPECT_EQ(p.lifecycle_state(), IProvider::lifecycle::error);
}

TEST(BitgetFuturesRegister, RejectsMalformedOrPercentageScaleLiquidationCap)
{
    for (const char* value : {"oops", "nan", "7"})
    {
        provider_config cfg;
        cfg["symbol"] = "BTCUSDT";
        cfg["min_liquidation_distance_pct"] = value;
        EXPECT_THROW(create(cfg), std::runtime_error) << value;
    }
}

// Credential completeness is checked before any risk/network setup.
TEST(BitgetFuturesRegister, MissingCredentialsRefuseBeforeRiskCheckConstruction)
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

    EXPECT_FALSE(p->open());
    EXPECT_EQ(p->lifecycle_state(), IProvider::lifecycle::error);
    EXPECT_EQ(p->get_risk_check(), nullptr);
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
    note = bitget::check_one_way_hold_mode(
        R"({"code":"00000","data":{"nested":{"holdMode":"one_way_mode"}}})");
    EXPECT_NE(note.find("holdMode"), std::string::npos);
    note = bitget::check_one_way_hold_mode(
        R"({"code":"00000","data":{"holdMode":"one_way_mode","holdMode":"hedge_mode"}})");
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

TEST(BitgetMarginTypeStrict, UnsupportedExpectedOrVenueModeRefuses)
{
    const char* portfolio =
        R"({"code":"00000","data":{"symbolConfigList":[)"
        R"({"symbol":"BTCUSDT","marginMode":"portfolio"}]}})";
    auto note = bitget::check_margin_type_strict(
        portfolio, "BTCUSDT", "portfolio");
    EXPECT_NE(note.find("unsupported expected"), std::string::npos);

    note = bitget::check_margin_type_strict(
        portfolio, "BTCUSDT", "CROSSED");
    EXPECT_NE(note.find("unsupported venue"), std::string::npos);
}

TEST(BitgetMarginTypeStrict, MissingSymbolRefuses)
{
    const char* body =
        R"({"code":"00000","data":{"symbolConfigList":[)"
        R"({"symbol":"ETHUSDT","marginMode":"crossed"}]}})";
    auto note = bitget::check_margin_type_strict(body, "BTCUSDT", "CROSSED");
    EXPECT_NE(note.find("not found"), std::string::npos);

    const char* nested =
        R"({"code":"00000","data":{"nested":{"symbolConfigList":[)"
        R"({"symbol":"BTCUSDT","marginMode":"crossed"}]}}})";
    note = bitget::check_margin_type_strict(nested, "BTCUSDT", "CROSSED");
    EXPECT_NE(note.find("not found"), std::string::npos);

    const char* nested_row =
        R"({"code":"00000","data":{"symbolConfigList":[)"
        R"({"nested":{"symbol":"BTCUSDT","marginMode":"crossed"}}]}})";
    note = bitget::check_margin_type_strict(
        nested_row, "BTCUSDT", "CROSSED");
    EXPECT_NE(note.find("not found"), std::string::npos);

    const char* duplicate_symbol =
        R"({"code":"00000","data":{"symbolConfigList":[)"
        R"({"symbol":"BTCUSDT","marginMode":"crossed"},)"
        R"({"symbol":"BTCUSDT","marginMode":"crossed"}]}})";
    note = bitget::check_margin_type_strict(
        duplicate_symbol, "BTCUSDT", "CROSSED");
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
