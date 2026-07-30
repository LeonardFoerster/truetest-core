#include <gtest/gtest.h>

#ifdef HAS_GATE

#include "providers/gate/gate_parser.h"

TEST(GateParser, NormalizeAlreadyCanonical)
{
    EXPECT_EQ(gate::normalize_contract_symbol("BTC_USDT"), "BTC_USDT");
    EXPECT_EQ(gate::normalize_contract_symbol("eth_usdt"), "ETH_USDT");
}

TEST(GateParser, NormalizeBareBtcUsdt)
{
    EXPECT_EQ(gate::normalize_contract_symbol("BTCUSDT"), "BTC_USDT");
    EXPECT_EQ(gate::normalize_contract_symbol("btcusdt"), "BTC_USDT");
}

TEST(GateParser, NormalizeOtherQuotes)
{
    EXPECT_EQ(gate::normalize_contract_symbol("ETHUSDC"), "ETH_USDC");
}

TEST(GateParser, ExtractStringField)
{
    constexpr auto json = R"({"channel":"futures.trades","event":"update"})";
    EXPECT_EQ(gate::extract_sv_string(json, "channel"), "futures.trades");
    EXPECT_EQ(gate::extract_sv_string(json, "event"), "update");
}

TEST(GateParser, ExtractBool)
{
    constexpr auto json = R"({"result":true,"dual":false})";
    EXPECT_TRUE(gate::extract_sv_bool(json, "result"));
    EXPECT_FALSE(gate::extract_sv_bool(json, "dual"));
    auto opt = gate::extract_sv_optional_bool(json, "missing");
    EXPECT_FALSE(opt.has_value());
}

#endif // HAS_GATE
