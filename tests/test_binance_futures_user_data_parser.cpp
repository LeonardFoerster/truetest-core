#include <gtest/gtest.h>

#include "providers/binance/binance_futures_user_data_parser.h"

#include <string>

namespace {

// Minimal ORDER_TRADE_UPDATE payload - wrapper carries event time `E`,
// inner `o:{...}` carries everything else. Real futures payloads have
// many more fields; the parser only needs these.
std::string update(const std::string& x, const std::string& X,
                   const std::string& l = "0.0",
                   const std::string& L = "0.0",
                   const std::string& z = "0.0",
                   const std::string& c = "tt-1",
                   const std::string& S = "BUY",
                   const std::string& i = "42",
                   const std::string& n = "0.0",
                   const std::string& N = "USDT",
                   const std::string& r = "NONE")
{
    std::string j = R"({"e":"ORDER_TRADE_UPDATE",)";
    j += R"("E":1700000000000,)";
    j += R"("T":1700000000000,)";
    j += R"("o":{)";
    j +=     R"("s":"BTCUSDT",)";
    j +=     R"("c":")" + c + R"(",)";
    j +=     R"("S":")" + S + R"(",)";
    j +=     R"("o":"LIMIT",)";  // inner order TYPE - must not collide with wrapper "o"
    j +=     R"("x":")" + x + R"(",)";
    j +=     R"("X":")" + X + R"(",)";
    j +=     R"("r":")" + r + R"(",)";
    j +=     R"("i":)" + i + ",";
    j +=     R"("l":")" + l + R"(",)";
    j +=     R"("L":")" + L + R"(",)";
    j +=     R"("z":")" + z + R"(",)";
    j +=     R"("n":")" + n + R"(",)";
    j +=     R"("N":")" + N + R"(",)";
    j +=     R"("T":1700000000001)";  // inner transaction time, distinct from wrapper E
    j += R"(}})";
    return j;
}

}

TEST(BinanceFuturesUserDataParser, RejectsNonOrderTradeUpdates)
{
    BinanceFuturesUserDataParser p;
    parsed_exec out;

    // ACCOUNT_UPDATE must NOT be picked up by parse() - it goes through
    // parse_position_snapshot() instead. This is the contract that lets
    // ExecutionBridge route the two event kinds to different consumers.
    std::string acct = R"({"e":"ACCOUNT_UPDATE","E":1})";
    EXPECT_EQ(p.parse(acct, out), execution_parse_result::unrelated);

    std::string listen = R"({"e":"listenKeyExpired","E":1})";
    EXPECT_EQ(p.parse(listen, out), execution_parse_result::malformed);

    std::string terminated = R"({"e":"eventStreamTerminated","E":1})";
    EXPECT_EQ(p.parse(terminated, out), execution_parse_result::malformed);

    std::string spot = R"({"e":"executionReport","E":1,"s":"X","c":"c","S":"BUY",)"
                       R"("x":"NEW","X":"NEW","i":1,"l":"0","L":"0","z":"0","n":"0","N":"USDT"})";
    EXPECT_EQ(p.parse(spot, out), execution_parse_result::unrelated);
}

TEST(BinanceFuturesUserDataParser, HarmlessControlsLeaveOutputUntouched)
{
    BinanceFuturesUserDataParser p;
    parsed_exec out;
    out.symbol = "sentinel";
    out.client_order_id = "keep";

    EXPECT_EQ(p.parse("ping", out), execution_parse_result::unrelated);
    EXPECT_EQ(p.parse("pong", out), execution_parse_result::unrelated);
    EXPECT_EQ(p.parse(R"({"result":null,"id":1})", out),
              execution_parse_result::unrelated);
    EXPECT_EQ(out.symbol, "sentinel");
    EXPECT_EQ(out.client_order_id, "keep");
}

TEST(BinanceFuturesUserDataParser,
     MalformedKnownEnvelopeFailsClosedAndPreservesOutput)
{
    BinanceFuturesUserDataParser p;
    parsed_exec out;
    out.symbol = "sentinel";
    out.client_order_id = "keep";

    EXPECT_EQ(p.parse(R"({"e":"ORDER_TRADE_UPDATE")", out),
              execution_parse_result::malformed);
    EXPECT_EQ(p.parse(R"({"e":"ORDER_TRADE_UPDATE"})", out),
              execution_parse_result::malformed);
    EXPECT_EQ(p.parse(R"({"o":{"s":"BTCUSDT","c":"tt-1","S":"BUY","x":"NEW","X":"NEW","i":42}})", out),
              execution_parse_result::malformed);
    EXPECT_EQ(p.parse(R"({"e":"bogus","o":{"s":"BTCUSDT","c":"tt-1","S":"BUY","x":"NEW","X":"NEW","i":42}})", out),
              execution_parse_result::malformed);
    EXPECT_EQ(p.parse(update("NEW", "FILLED"), out),
              execution_parse_result::malformed);
    EXPECT_EQ(p.parse(update("TRADE", "FILLED", "nan", "100", "1"), out),
              execution_parse_result::malformed);
    EXPECT_EQ(p.parse(update("NEW", "NEW", "0", "0", "0", "tt-1", "HOLD"), out),
              execution_parse_result::malformed);
    auto missing_fee = update("TRADE", "FILLED", "1", "100", "1");
    const auto fee = missing_fee.find(R"(,"n":"0.0")");
    ASSERT_NE(fee, std::string::npos);
    missing_fee.erase(fee, std::string(R"(,"n":"0.0")").size());
    EXPECT_EQ(p.parse(missing_fee, out), execution_parse_result::malformed);
    auto missing_order_id = update("NEW", "NEW");
    const auto order_id = missing_order_id.find(R"("i":42,)");
    ASSERT_NE(order_id, std::string::npos);
    missing_order_id.erase(order_id, std::string(R"("i":42,)").size());
    EXPECT_EQ(p.parse(missing_order_id, out), execution_parse_result::malformed);
    auto nonnumeric_order_id = update("NEW", "NEW");
    const auto numeric_order_id = nonnumeric_order_id.find(R"("i":42)");
    ASSERT_NE(numeric_order_id, std::string::npos);
    nonnumeric_order_id.replace(numeric_order_id, std::string(R"("i":42)").size(),
                                R"("i":"alpha-7")");
    EXPECT_EQ(p.parse(nonnumeric_order_id, out), execution_parse_result::malformed);
    auto empty_fee = update("NEW", "NEW");
    const auto empty = empty_fee.find(R"("n":"0.0")");
    ASSERT_NE(empty, std::string::npos);
    empty_fee.replace(empty, std::string(R"("n":"0.0")").size(),
                      R"("n":"")");
    EXPECT_EQ(p.parse(empty_fee, out), execution_parse_result::malformed);
    auto overflow_ts = update("NEW", "NEW");
    const auto timestamp = overflow_ts.find("1700000000000");
    ASSERT_NE(timestamp, std::string::npos);
    overflow_ts.replace(timestamp, std::string("1700000000000").size(),
                        "9223372036854775807");
    EXPECT_EQ(p.parse(overflow_ts, out), execution_parse_result::malformed);
    EXPECT_EQ(out.symbol, "sentinel");
    EXPECT_EQ(out.client_order_id, "keep");
}

TEST(BinanceFuturesUserDataParser, NullableCommissionAssetIsAccepted)
{
    BinanceFuturesUserDataParser p;
    parsed_exec out;
    auto j = update("NEW", "NEW");
    const auto field = j.find(R"("N":"USDT")");
    ASSERT_NE(field, std::string::npos);
    j.replace(field, std::string(R"("N":"USDT")").size(), R"("N":null)");

    ASSERT_EQ(p.parse(j, out), execution_parse_result::valid);
    EXPECT_TRUE(out.commission_asset.empty());
}

TEST(BinanceFuturesUserDataParser, NonzeroCommissionRequiresAsset)
{
    BinanceFuturesUserDataParser p;
    parsed_exec out;
    auto j = update("TRADE", "FILLED", "1", "100", "1", "tt-1",
                    "BUY", "42", "0.01", "USDT");
    const auto field = j.find(R"("N":"USDT")");
    ASSERT_NE(field, std::string::npos);
    j.replace(field, std::string(R"("N":"USDT")").size(), R"("N":null)");

    EXPECT_EQ(p.parse(j, out), execution_parse_result::malformed);
}

TEST(BinanceFuturesUserDataParser, FundingParserDoesNotPreemptOrderText)
{
    BinanceFuturesUserDataParser p;
    parsed_funding_update funding;
    const auto order = update("NEW", "NEW", "0", "0", "0",
                              "FUNDING_FEE-client");

    EXPECT_EQ(p.parse_funding_update(order, funding),
              funding_parse_result::not_funding);
}

TEST(BinanceFuturesUserDataParser, FundingTimestampMustFitSystemClock)
{
    BinanceFuturesUserDataParser p;
    parsed_funding_update funding;
    const std::string overflow =
        R"({"e":"ACCOUNT_UPDATE","E":9223372036854775807,"a":{"m":"FUNDING_FEE","B":[{"a":"USDT","bc":"-0.5"}]}})";

    EXPECT_EQ(p.parse_funding_update(overflow, funding),
              funding_parse_result::invalid);
}

TEST(BinanceFuturesUserDataParser, AccountUpdateOrderReasonWithPositionAndBalance)
{
    BinanceFuturesUserDataParser p;
    parsed_position_snapshot s;

    std::string j = R"({"e":"ACCOUNT_UPDATE","E":1700000000000,"T":1700000000000,)"
                    R"("a":{"m":"ORDER",)"
                    R"("B":[{"a":"USDT","wb":"122624.12","cw":"100.12","bc":"50.12"}],)"
                    R"("P":[{"s":"BTCUSDT","pa":"0.5","ep":"30000","mt":"isolated","ps":"BOTH"}]}})";

    ASSERT_TRUE(p.parse_position_snapshot(j, s));
    EXPECT_EQ(s.r, parsed_position_snapshot::reason::order);
    ASSERT_EQ(s.balances.size(), 1u);
    EXPECT_EQ(s.balances[0].asset, "USDT");
    EXPECT_DOUBLE_EQ(s.balances[0].wallet_balance, 122624.12);
    EXPECT_DOUBLE_EQ(s.balances[0].balance_change, 50.12);

    ASSERT_EQ(s.positions.size(), 1u);
    EXPECT_EQ(s.positions[0].symbol, "BTCUSDT");
    EXPECT_DOUBLE_EQ(s.positions[0].qty, 0.5);
    EXPECT_EQ(s.positions[0].margin_type, "ISOLATED");
    EXPECT_EQ(s.positions[0].position_side, "BOTH");

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  s.ts.time_since_epoch()).count();
    EXPECT_EQ(ms, 1700000000000LL);
}

TEST(BinanceFuturesUserDataParser, AccountUpdateNegativePositionAmtIsShort)
{
    BinanceFuturesUserDataParser p;
    parsed_position_snapshot s;

    std::string j = R"({"e":"ACCOUNT_UPDATE","E":1,)"
                    R"("a":{"m":"ORDER","B":[],)"
                    R"("P":[{"s":"BTCUSDT","pa":"-1.25","mt":"cross","ps":"BOTH"}]}})";

    ASSERT_TRUE(p.parse_position_snapshot(j, s));
    ASSERT_EQ(s.positions.size(), 1u);
    EXPECT_DOUBLE_EQ(s.positions[0].qty, -1.25);
    EXPECT_EQ(s.positions[0].margin_type, "CROSSED");
}

TEST(BinanceFuturesUserDataParser, AccountUpdateFundingFeeReason)
{
    BinanceFuturesUserDataParser p;
    parsed_position_snapshot s;

    std::string j = R"({"e":"ACCOUNT_UPDATE","E":1,)"
                    R"("a":{"m":"FUNDING_FEE",)"
                    R"("B":[{"a":"USDT","wb":"99.5","bc":"-0.5"}],)"
                    R"("P":[]}})";

    ASSERT_TRUE(p.parse_position_snapshot(j, s));
    EXPECT_EQ(s.r, parsed_position_snapshot::reason::funding_fee);
    ASSERT_EQ(s.balances.size(), 1u);
    EXPECT_DOUBLE_EQ(s.balances[0].balance_change, -0.5);
    EXPECT_TRUE(s.positions.empty());
}

TEST(BinanceFuturesUserDataParser, AccountUpdateLiquidationReason)
{
    BinanceFuturesUserDataParser p;
    parsed_position_snapshot s;

    std::string j = R"({"e":"ACCOUNT_UPDATE","E":1,)"
                    R"("a":{"m":"INSURANCE_CLEAR","B":[],"P":[]}})";

    ASSERT_TRUE(p.parse_position_snapshot(j, s));
    EXPECT_EQ(s.r, parsed_position_snapshot::reason::liquidation);
}

TEST(BinanceFuturesUserDataParser, AccountUpdateUnknownReasonFallsBack)
{
    BinanceFuturesUserDataParser p;
    parsed_position_snapshot s;

    std::string j = R"({"e":"ACCOUNT_UPDATE","E":1,)"
                    R"("a":{"m":"WEIRD_NEW_REASON","B":[],"P":[]}})";

    ASSERT_TRUE(p.parse_position_snapshot(j, s));
    EXPECT_EQ(s.r, parsed_position_snapshot::reason::other);
}

TEST(BinanceFuturesUserDataParser, AccountUpdateMissingFieldEmptyArrays)
{
    BinanceFuturesUserDataParser p;
    parsed_position_snapshot s;

    std::string j = R"({"e":"ACCOUNT_UPDATE","E":1,"a":{"m":"ORDER"}})";

    // No B[] or P[] keys: parse should still succeed and leave the
    // vectors empty rather than rejecting outright. Real responses
    // sometimes omit one of the arrays when only the other changed.
    ASSERT_TRUE(p.parse_position_snapshot(j, s));
    EXPECT_TRUE(s.balances.empty());
    EXPECT_TRUE(s.positions.empty());
}

TEST(BinanceFuturesUserDataParser, ParsePositionSnapshotRejectsOrderTradeUpdate)
{
    BinanceFuturesUserDataParser p;
    parsed_position_snapshot s;

    std::string j = R"({"e":"ORDER_TRADE_UPDATE","E":1,"o":{"s":"BTCUSDT"}})";

    EXPECT_FALSE(p.parse_position_snapshot(j, s));
}

TEST(BinanceFuturesUserDataParser, NewAck)
{
    BinanceFuturesUserDataParser p;
    parsed_exec out;
    auto j = update("NEW", "NEW");

    ASSERT_EQ(p.parse(j, out), execution_parse_result::valid);
    EXPECT_EQ(out.k, parsed_exec::kind::ack);
    EXPECT_EQ(out.symbol, "BTCUSDT");
    EXPECT_EQ(out.client_order_id, "tt-1");
    EXPECT_EQ(out.exchange_order_id, "42");
    EXPECT_EQ(out.side, order_side::buy);
    EXPECT_DOUBLE_EQ(out.last_fill_qty, 0.0);
}

TEST(BinanceFuturesUserDataParser, PartialTrade)
{
    BinanceFuturesUserDataParser p;
    parsed_exec out;
    auto j = update("TRADE", "PARTIALLY_FILLED",
                    "0.4", "60000.0", "0.4");

    ASSERT_EQ(p.parse(j, out), execution_parse_result::valid);
    EXPECT_EQ(out.k, parsed_exec::kind::partial_fill);
    EXPECT_DOUBLE_EQ(out.last_fill_qty, 0.4);
    EXPECT_DOUBLE_EQ(out.last_fill_price, 60000.0);
    EXPECT_DOUBLE_EQ(out.cumulative_qty, 0.4);
}

TEST(BinanceFuturesUserDataParser, FullTrade)
{
    BinanceFuturesUserDataParser p;
    parsed_exec out;
    auto j = update("TRADE", "FILLED",
                    "0.6", "60010.0", "1.0",
                    "tt-9", "SELL", "77",
                    "0.06", "USDT");

    ASSERT_EQ(p.parse(j, out), execution_parse_result::valid);
    EXPECT_EQ(out.k, parsed_exec::kind::full_fill);
    EXPECT_EQ(out.side, order_side::sell);
    EXPECT_EQ(out.client_order_id, "tt-9");
    EXPECT_EQ(out.exchange_order_id, "77");
    EXPECT_DOUBLE_EQ(out.last_fill_qty, 0.6);
    EXPECT_DOUBLE_EQ(out.last_fill_price, 60010.0);
    EXPECT_DOUBLE_EQ(out.cumulative_qty, 1.0);
    EXPECT_DOUBLE_EQ(out.commission, 0.06);
    EXPECT_EQ(out.commission_asset, "USDT");
}

TEST(BinanceFuturesUserDataParser, Canceled)
{
    BinanceFuturesUserDataParser p;
    parsed_exec out;
    ASSERT_EQ(p.parse(update("CANCELED", "CANCELED"), out),
              execution_parse_result::valid);
    EXPECT_EQ(out.k, parsed_exec::kind::canceled);
}

TEST(BinanceFuturesUserDataParser, Rejected)
{
    BinanceFuturesUserDataParser p;
    parsed_exec out;
    auto j = update("REJECTED", "REJECTED", "0", "0", "0",
                    "tt-2", "BUY", "0", "0", "USDT",
                    "INSUFFICIENT_MARGIN");
    ASSERT_EQ(p.parse(j, out), execution_parse_result::valid);
    EXPECT_EQ(out.k, parsed_exec::kind::rejected);
    EXPECT_EQ(out.error, "INSUFFICIENT_MARGIN");
}

TEST(BinanceFuturesUserDataParser, Expired)
{
    BinanceFuturesUserDataParser p;
    parsed_exec out;
    ASSERT_EQ(p.parse(update("EXPIRED", "EXPIRED"), out),
              execution_parse_result::valid);
    EXPECT_EQ(out.k, parsed_exec::kind::expired);
}

TEST(BinanceFuturesUserDataParser, StpExpiryRemainsValid)
{
    BinanceFuturesUserDataParser p;
    parsed_exec out;

    ASSERT_EQ(p.parse(update("EXPIRED", "EXPIRED_IN_MATCH"), out),
              execution_parse_result::valid);
    EXPECT_EQ(out.k, parsed_exec::kind::expired);
}

TEST(BinanceFuturesUserDataParser, CalculatedLiquidationFillRemainsValid)
{
    BinanceFuturesUserDataParser p;
    parsed_exec out;

    ASSERT_EQ(p.parse(update("CALCULATED", "FILLED", "1", "100", "1"), out),
              execution_parse_result::valid);
    EXPECT_EQ(out.k, parsed_exec::kind::full_fill);
}

TEST(BinanceFuturesUserDataParser, UnsupportedConditionalLifecyclesFailClosed)
{
    BinanceFuturesUserDataParser p;
    parsed_exec out;

    EXPECT_EQ(p.parse(
                  R"({"e":"CONDITIONAL_ORDER_TRIGGER_REJECT","E":1700000000000})",
                  out),
              execution_parse_result::malformed);
    EXPECT_EQ(p.parse(R"({"e":"ALGO_UPDATE","E":1700000000000})", out),
              execution_parse_result::malformed);
    EXPECT_EQ(p.parse(R"({"e":"TRADE_LITE","E":1700000000000})", out),
              execution_parse_result::malformed);
}

TEST(BinanceFuturesUserDataParser, TimestampFromWrapperEventMillis)
{
    BinanceFuturesUserDataParser p;
    parsed_exec out;
    ASSERT_EQ(p.parse(update("NEW", "NEW"), out),
              execution_parse_result::valid);

    // Wrapper `E`, not inner `T`, drives the timestamp (matches spot).
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  out.ts.time_since_epoch()).count();
    EXPECT_EQ(ms, 1700000000000LL);
}

TEST(BinanceFuturesUserDataParser, NumericStringExchangeIdIsAccepted)
{
    BinanceFuturesUserDataParser p;
    parsed_exec out;
    std::string j = R"({"e":"ORDER_TRADE_UPDATE","E":1,"o":{)"
                    R"("s":"X","c":"c1","S":"BUY","x":"NEW","X":"NEW",)"
                    R"("i":"77","l":"0","L":"0","z":"0","n":"0","N":"USDT"}})";
    ASSERT_EQ(p.parse(j, out), execution_parse_result::valid);
    EXPECT_EQ(out.exchange_order_id, "77");
}
