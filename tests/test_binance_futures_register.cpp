#include <gtest/gtest.h>

#ifdef HAS_BINANCE

#include "providers/provider_registry.h"
#include "providers/binance/binance_futures_provider.h"
#include "helpers/alloc_counter.h"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

std::shared_ptr<BinanceFuturesProvider> create(const provider_config& cfg)
{
    auto p = ProviderRegistry::instance().create("binance-futures", cfg);
    return std::dynamic_pointer_cast<BinanceFuturesProvider>(p);
}

class FeedTransportStub final : public IDataTransport
{
public:
    bool open() override { return true; }
    void close() override {}
    bool is_open() const override { return true; }
    std::optional<std::string> read_line() override { return std::nullopt; }
};

class RenamedBinanceFuturesProviderForTest final
    : public BinanceFuturesProvider
{
public:
    using BinanceFuturesProvider::BinanceFuturesProvider;

    std::string name() const override { return "opaque-venue-id"; }

    std::shared_ptr<IDataTransport> get_transport() override
    {
        return transport_;
    }

private:
    std::shared_ptr<IDataTransport> transport_ =
        std::make_shared<FeedTransportStub>();
};

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

TEST(BinanceFuturesRegister, C07_KlineOnlyProviderOwnsCompleteEventFeedWithoutDepth)
{
    // Deliberately omit depth_stream and hide the registry name. The provider
    // still emits Binance JSON, so its decoder contract must be provider-owned.
    RenamedBinanceFuturesProviderForTest p("BTCUSDT", "kline_1m");

    EXPECT_TRUE(p.supports_event_stream());
    auto feed = p.get_market_data_feed();
    ASSERT_TRUE(feed.has_value());
    ASSERT_TRUE(feed->ready());
    ASSERT_TRUE(feed->request.has_value());
    EXPECT_EQ(feed->request->symbol, "BTCUSDT");
    ASSERT_EQ(feed->request->channels.size(), 1U);
    EXPECT_EQ(feed->request->channels.front().kind,
              market_data_channel_kind::candles);
    ASSERT_TRUE(feed->capabilities.has_value());
    EXPECT_TRUE(feed->capabilities->candles);
    EXPECT_FALSE(feed->capabilities->trades);
    EXPECT_FALSE(feed->capabilities->l2_snapshots);
    EXPECT_FALSE(feed->capabilities->l2_deltas);

    const std::string frame =
        R"({"e":"kline","E":1704067260000,"s":"BTCUSDT","k":)"
        R"({"t":1704067200000,"T":1704067259999,"s":"BTCUSDT","i":"1m",)"
        R"("o":"42000","c":"42100","h":"42200","l":"41900","v":"5.0","x":true}})";
    auto event = feed->parser->parse_record(frame);
    ASSERT_TRUE(event.has_value());
    ASSERT_TRUE(std::holds_alternative<provider::bar>(*event));
    const auto& bar = std::get<provider::bar>(*event);
    EXPECT_EQ(bar.symbol, "BTCUSDT");
    EXPECT_DOUBLE_EQ(bar.open, 42000.0);
    EXPECT_DOUBLE_EQ(bar.high, 42200.0);
    EXPECT_DOUBLE_EQ(bar.low, 41900.0);
    EXPECT_DOUBLE_EQ(bar.close, 42100.0);
    EXPECT_EQ(bar.volume, 500'000'000);
}

TEST(BinanceFuturesRegister, C14_BackfillEncodingPreservesSymbolAndCausalCloseTime)
{
    const backfill_bar source{
        .open_time = 1'704'067'200'000,
        .close_time = 1'704'067'259'999,
        .open = 42'000.0,
        .high = 42'200.0,
        .low = 41'900.0,
        .close = 42'100.0,
        .volume = 5.0,
    };
    const auto payload = binance::encode_backfill_kline_json(source, "btcusdt", "1m");
    ASSERT_TRUE(payload.has_value());

    BinanceCombinedParser parser;
    const auto parsed = parser.parse_record(*payload);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_TRUE(std::holds_alternative<provider::bar>(*parsed));
    const auto& parsed_bar = std::get<provider::bar>(*parsed);
    EXPECT_EQ(parsed_bar.symbol, "BTCUSDT")
        << "backfill and live frames must address the same symbol state";
    EXPECT_EQ(parsed_bar.date, "1704067259999");

    auto invalid = source;
    invalid.close_time = 0;
    EXPECT_FALSE(binance::encode_backfill_kline_json(invalid, "BTCUSDT", "1m").has_value());
    EXPECT_FALSE(binance::encode_backfill_kline_json(source, "", "1m").has_value());
    EXPECT_FALSE(binance::encode_backfill_kline_json(source, "BTC USDT", "1m").has_value());
    EXPECT_FALSE(
        binance::encode_backfill_kline_json(source, std::string(129, 'A'), "1m").has_value());

    invalid = source;
    invalid.close_time -= 1;
    EXPECT_FALSE(binance::encode_backfill_kline_json(
        invalid, "BTCUSDT", "1m").has_value());
    EXPECT_FALSE(binance::encode_backfill_kline_json(
        source, "BTCUSDT", "1M").has_value());
}

TEST(BinanceFuturesRegister, BackfillFetchIsUnsupportedBeforeNetworkIo)
{
    BinanceBackfill backfill("must-not-be-contacted.invalid");
    EXPECT_THROW(
        (void)backfill.fetch("BTCUSDT", "1m", 1),
        std::logic_error);
    EXPECT_TRUE(backfill.fetch("BTCUSDT", "1m", 0).empty());
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
// futures testnet WS host - looks_like_testnet_host() must catch it on
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

TEST(BinanceFuturesRegister, FundingParserPublishesThroughProviderIngress)
{
    BinanceFuturesUserDataParser parser;
    ProviderFundingIngress ingress;
    constexpr std::string_view frame =
        R"({"e":"ACCOUNT_UPDATE","E":1700000000000,"a":{"m":"FUNDING_FEE","B":[{"a":"USDT","wb":"99.5","bc":"-0.5"}],"P":[]}})";
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
                && popped.cash_delta == -0.5;
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
    EXPECT_DOUBLE_EQ(update.cash_delta, -0.5);
    EXPECT_FALSE(ingress.try_pop(update));
}

TEST(BinanceFuturesRegister, FundingParserRefusesAmbiguousAccountingEvidence)
{
    BinanceFuturesUserDataParser parser;
    parsed_funding_update parsed;
    for (const auto body : {
        R"({"e":"ACCOUNT_UPDATE","E":1700000000000,"a":{"m":"FUNDING_FEE","B":[]}})",
        R"({"e":"ACCOUNT_UPDATE","E":1700000000000,"a":{"m":"FUNDING_FEE","B":[{"a":"USDT","bc":"1junk"}]}})",
        R"({"e":"ACCOUNT_UPDATE","E":1700000000000,"a":{"m":"FUNDING_FEE","B":[{"a":"USDT","bc":"0"}]}})",
        R"({"e":"ACCOUNT_UPDATE","E":1700000000000,"a":{"m":"FUNDING_FEE","m":"ORDER","B":[{"a":"USDT","bc":"1"}]}})",
        R"({"e":"ACCOUNT_UPDATE","E":1700000000000,"a":{"m":"FUNDING_FEE","B":[{"a":"USDT","bc":"1","bc":"2"}]}})",
        R"({"e":"ACCOUNT_UPDATE","E":1700000000000,"a":{"m":"FUNDING_FEE","B":[{"a":"USDT","bc":"1"},{"a":"USDT","bc":"2"}]}})"})
        EXPECT_EQ(parser.parse_funding_update(body, parsed),
                  funding_parse_result::invalid);

    EXPECT_EQ(parser.parse_funding_update(
        R"({"e":"ACCOUNT_UPDATE","E":1700000000000,"a":{"m":"ORDER","B":[{"a":"USDT","bc":"1"}]}})",
        parsed), funding_parse_result::not_funding);
}

TEST(BinanceFuturesRegister, PositionModeResponseIsAuthoritativeTopLevelBool)
{
    auto one_way = binance::authoritative_dual_side_position(
        R"({"dualSidePosition":false})");
    ASSERT_TRUE(one_way.has_value());
    EXPECT_FALSE(*one_way);

    auto hedge = binance::authoritative_dual_side_position(
        R"({"dualSidePosition":true})");
    ASSERT_TRUE(hedge.has_value());
    EXPECT_TRUE(*hedge);

    EXPECT_FALSE(binance::authoritative_dual_side_position(
        R"({"nested":{"dualSidePosition":false},"dualSidePosition":true}) trailing)").has_value());
    EXPECT_FALSE(binance::authoritative_dual_side_position(
        R"({"nested":{"dualSidePosition":false}})").has_value());
    EXPECT_FALSE(binance::authoritative_dual_side_position(
        R"({"dualSidePosition":false,"dualSidePosition":true})").has_value());
    EXPECT_FALSE(binance::authoritative_dual_side_position(
        R"({"dualSidePosition":"false"})").has_value());

    auto top_level_true = binance::authoritative_dual_side_position(
        R"({"nested":{"dualSidePosition":false},"dualSidePosition":true})");
    ASSERT_TRUE(top_level_true.has_value());
    EXPECT_TRUE(*top_level_true);
    auto top_level_false = binance::authoritative_dual_side_position(
        R"({"nested":{"dualSidePosition":true},"dualSidePosition":false})");
    ASSERT_TRUE(top_level_false.has_value());
    EXPECT_FALSE(*top_level_false);
}

TEST(BinanceFuturesRegister, DirectLiveOpenRefusesEveryIncompleteCredentialPair)
{
    for (unsigned mask = 0; mask < 3; ++mask)
    {
        provider_config cfg;
        cfg["symbol"] = "btcusdt";
        if (mask & 1u) cfg["api_key"] = "key";
        if (mask & 2u) cfg["api_secret"] = "secret";
        auto p = create(cfg);
        ASSERT_NE(p, nullptr);
        engine_config ec;
        ec.mode = engine_mode::live;
        p->configure(ec);
        EXPECT_FALSE(p->open()) << "mask=" << mask;
        EXPECT_EQ(p->lifecycle_state(), IProvider::lifecycle::error);
    }
}

TEST(BinanceFuturesRegister, LiveSafetyPreparationIsOperationalWithoutOpen)
{
    BinanceFuturesProvider p(
        "btcusdt", "trade", "test-key", "test-secret");
    engine_config cfg;
    cfg.mode = engine_mode::live;
    p.configure(cfg);

    EXPECT_EQ(p.private_execution_capability_level(),
              private_execution_capability::exchange_writes);
    ASSERT_TRUE(p.prepare_write_safety());
    ASSERT_NE(p.get_reconciler(), nullptr);
    ASSERT_NE(p.get_kill_switch(), nullptr);
    EXPECT_TRUE(p.get_reconciler()->is_operational());
    EXPECT_TRUE(p.get_kill_switch()->is_operational());
    EXPECT_EQ(p.lifecycle_state(), IProvider::lifecycle::closed);
}

TEST(BinanceFuturesRegister, DirectLiveOpenCannotBypassSafetySession)
{
    BinanceFuturesProvider p(
        "btcusdt", "trade", "test-key", "test-secret");
    engine_config cfg;
    cfg.mode = engine_mode::live;
    p.configure(cfg);

    EXPECT_FALSE(p.open());
    EXPECT_EQ(p.lifecycle_state(), IProvider::lifecycle::error);
}

TEST(BinanceFuturesRegister, RejectsMalformedOrPercentageScaleLiquidationCap)
{
    for (const char* value : {"oops", "nan", "7"})
    {
        provider_config cfg;
        cfg["symbol"] = "btcusdt";
        cfg["min_liquidation_distance_pct"] = value;
        EXPECT_THROW(create(cfg), std::runtime_error) << value;
    }
}

#endif // HAS_BINANCE
