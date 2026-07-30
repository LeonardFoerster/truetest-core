#include <gtest/gtest.h>

#ifdef HAS_BYBIT

#include "providers/bybit/bybit_futures_user_data_parser.h"
#include "providers/bybit/bybit_user_data_transport.h"

#include <string>

namespace {

// V5 order-channel push (canned from docs shape / plan §8.5).
std::string order_push(const std::string& status,
                       const std::string& cum = "0",
                       const std::string& side = "Buy",
                       const std::string& client = "tt-1",
                       const std::string& order_id = "42")
{
    std::string j = R"({"topic":"order.linear","type":"snapshot","ts":1700000000001,"data":[{)";
    j += R"("category":"linear",)";
    j += R"("symbol":"BTCUSDT",)";
    j += R"("orderId":")" + order_id + R"(",)";
    j += R"("orderLinkId":")" + client + R"(",)";
    j += R"("side":")" + side + R"(",)";
    j += R"("orderType":"Limit",)";
    j += R"("cumExecQty":")" + cum + R"(",)";
    j += R"("avgPrice":"60000",)";
    j += R"("orderStatus":")" + status + R"(",)";
    j += R"("rejectReason":"EC_NoError",)";
    j += R"("updatedTime":"1700000000000")";
    j += R"(}]})";
    return j;
}

std::string execution_push(const std::string& qty = "0.4",
                           const std::string& px = "60000",
                           const std::string& side = "Buy",
                           const std::string& leaves = "0.6",
                           const std::string& client = "tt-1",
                           const std::string& order_id = "42")
{
    std::string j = R"({"topic":"execution.linear","type":"snapshot","ts":1700000000003,"data":[{)";
    j += R"("symbol":"BTCUSDT",)";
    j += R"("orderId":")" + order_id + R"(",)";
    j += R"("orderLinkId":")" + client + R"(",)";
    j += R"("side":")" + side + R"(",)";
    j += R"("execQty":")" + qty + R"(",)";
    j += R"("execPrice":")" + px + R"(",)";
    j += R"("leavesQty":")" + leaves + R"(",)";
    j += R"("execType":"Trade",)";
    j += R"("execFee":"0.024",)";
    j += R"("feeCurrency":"USDT",)";
    j += R"("execTime":"1700000000002",)";
    j += R"("category":"linear")";
    j += R"(}]})";
    return j;
}

std::string position_push(const std::string& size,
                          const std::string& side,
                          const std::string& idx = "0")
{
    std::string j = R"({"topic":"position.linear","type":"snapshot","ts":1700000000011,"data":[{)";
    j += R"("symbol":"BTCUSDT",)";
    j += R"("size":")" + size + R"(",)";
    j += R"("side":")" + side + R"(",)";
    j += R"("positionIdx":)" + idx + R"(,)";
    j += R"("tradeMode":0,)";
    j += R"("updatedTime":"1700000000010")";
    j += R"(}]})";
    return j;
}

} // namespace

// --- Pure transport helpers ---

TEST(BybitUserDataTransportHelpers, AuthJsonExact)
{
    const std::string j = bybit::build_auth_json(
        "key", 1662350400000LL, "SIG");
    EXPECT_EQ(
        j,
        R"({"op":"auth","args":["key",1662350400000,"SIG"]})");
}

TEST(BybitUserDataTransportHelpers, AuthJsonEscapesSpecialChars)
{
    const std::string j = bybit::build_auth_json(
        R"(k"ey)", 1662350400000LL, "SI/G+=");
    EXPECT_EQ(
        j,
        R"({"op":"auth","args":["k\"ey",1662350400000,"SI/G+="]})");
}

TEST(BybitUserDataTransportHelpers, PrivateSubscribeExact)
{
    EXPECT_EQ(
        bybit::build_private_subscribe_json(),
        R"({"op":"subscribe","args":["order.linear","execution.linear","position.linear","wallet"]})");
}

TEST(BybitUserDataTransportHelpers, AuthSuccessCodes)
{
    EXPECT_TRUE(bybit::is_auth_success(
        R"({"success":true,"ret_msg":"","op":"auth","conn_id":"x"})"));
    EXPECT_TRUE(bybit::is_auth_success(
        R"({"op":"auth","retCode":0,"success":true})"));
    EXPECT_FALSE(bybit::is_auth_success(
        R"({"success":false,"ret_msg":"error","op":"auth"})"));
    EXPECT_FALSE(bybit::is_auth_success(R"({"op":"subscribe"})"));
    EXPECT_TRUE(bybit::is_auth_failure(
        R"({"success":false,"ret_msg":"bad","op":"auth"})"));
}

TEST(BybitUserDataTransport, ConstructDoesNotOpen)
{
    BybitUserDataTransport tx("k", "s");
    EXPECT_EQ(tx.state(), IFillTransport::lifecycle::closed);
}

TEST(BybitUserDataTransport, OpenWithoutCredentialsErrors)
{
    BybitUserDataTransport tx("", "");
    EXPECT_FALSE(tx.open());
    EXPECT_EQ(tx.state(), IFillTransport::lifecycle::error);
}

// --- orderStatus → kind (plan §8.5) ---

TEST(BybitFuturesUserDataParser, OrderStatusNewIsAck)
{
    BybitFuturesUserDataParser p;
    parsed_exec out;
    ASSERT_TRUE(p.parse(order_push("New"), out));
    EXPECT_EQ(out.k, parsed_exec::kind::ack);
    EXPECT_EQ(out.symbol, "BTCUSDT");
    EXPECT_EQ(out.client_order_id, "tt-1");
    EXPECT_EQ(out.exchange_order_id, "42");
    EXPECT_EQ(out.side, order_side::buy);
    EXPECT_DOUBLE_EQ(out.last_fill_qty, 0.0);
}

TEST(BybitFuturesUserDataParser, OrderStatusCancelled)
{
    BybitFuturesUserDataParser p;
    parsed_exec out;
    ASSERT_TRUE(p.parse(order_push("Cancelled"), out));
    EXPECT_EQ(out.k, parsed_exec::kind::canceled);
}

TEST(BybitFuturesUserDataParser, OrderStatusRejected)
{
    BybitFuturesUserDataParser p;
    parsed_exec out;
    ASSERT_TRUE(p.parse(order_push("Rejected"), out));
    EXPECT_EQ(out.k, parsed_exec::kind::rejected);
}

TEST(BybitFuturesUserDataParser, OrderFilledDemotedWithoutLastFill)
{
    // Dual-channel: order Filled with last_fill=0 demotes to other.
    BybitFuturesUserDataParser p;
    parsed_exec out;
    ASSERT_TRUE(p.parse(order_push("Filled", "1.0"), out));
    EXPECT_EQ(out.k, parsed_exec::kind::other);
    EXPECT_DOUBLE_EQ(out.cumulative_qty, 1.0);
    EXPECT_DOUBLE_EQ(out.last_fill_qty, 0.0);
}

// --- execution topic ---

TEST(BybitFuturesUserDataParser, ExecutionPartialFill)
{
    BybitFuturesUserDataParser p;
    parsed_exec out;
    ASSERT_TRUE(p.parse(execution_push("0.4", "60000", "Buy", "0.6"), out));
    EXPECT_EQ(out.k, parsed_exec::kind::partial_fill);
    EXPECT_DOUBLE_EQ(out.last_fill_qty, 0.4);
    EXPECT_DOUBLE_EQ(out.last_fill_price, 60000.0);
    EXPECT_DOUBLE_EQ(out.commission, 0.024);
    EXPECT_EQ(out.commission_asset, "USDT");
    EXPECT_EQ(out.side, order_side::buy);
}

TEST(BybitFuturesUserDataParser, ExecutionFullFillViaLeavesZero)
{
    BybitFuturesUserDataParser p;
    parsed_exec out;
    ASSERT_TRUE(p.parse(execution_push("1.0", "60000", "Sell", "0"), out));
    EXPECT_EQ(out.k, parsed_exec::kind::full_fill);
    EXPECT_EQ(out.side, order_side::sell);
    EXPECT_DOUBLE_EQ(out.last_fill_qty, 1.0);
}

TEST(BybitFuturesUserDataParser, FundingNotParsedAsExec)
{
    BybitFuturesUserDataParser p;
    parsed_exec out;
    const char* fund =
        R"({"topic":"execution.linear","data":[{"symbol":"BTCUSDT",)"
        R"("orderId":"0","orderLinkId":"","execType":"Funding",)"
        R"("execFee":"0.01","execQty":"0","execPrice":"0"}]})";
    EXPECT_FALSE(p.parse(fund, out));

    parsed_position_snapshot snap;
    ASSERT_TRUE(p.parse_position_snapshot(fund, snap));
    EXPECT_EQ(snap.r, parsed_position_snapshot::reason::funding_fee);
    ASSERT_FALSE(snap.balances.empty());
    EXPECT_DOUBLE_EQ(snap.balances[0].balance_change, -0.01);
}

// --- position / wallet ---

TEST(BybitFuturesUserDataParser, PositionLongSigned)
{
    BybitFuturesUserDataParser p;
    parsed_position_snapshot out;
    ASSERT_TRUE(p.parse_position_snapshot(position_push("0.5", "Buy", "0"), out));
    ASSERT_EQ(out.positions.size(), 1u);
    EXPECT_EQ(out.positions[0].symbol, "BTCUSDT");
    EXPECT_DOUBLE_EQ(out.positions[0].qty, 0.5);
    EXPECT_EQ(out.positions[0].position_side, "BOTH");
}

TEST(BybitFuturesUserDataParser, PositionShortSigned)
{
    BybitFuturesUserDataParser p;
    parsed_position_snapshot out;
    ASSERT_TRUE(p.parse_position_snapshot(position_push("1.2", "Sell", "0"), out));
    ASSERT_EQ(out.positions.size(), 1u);
    EXPECT_DOUBLE_EQ(out.positions[0].qty, -1.2);
}

TEST(BybitFuturesUserDataParser, PositionIdxHedgeMapping)
{
    EXPECT_EQ(BybitFuturesUserDataParser::position_side_from_idx("0"), "BOTH");
    EXPECT_EQ(BybitFuturesUserDataParser::position_side_from_idx("1"), "LONG");
    EXPECT_EQ(BybitFuturesUserDataParser::position_side_from_idx("2"), "SHORT");
}

TEST(BybitFuturesUserDataParser, WalletBalances)
{
    BybitFuturesUserDataParser p;
    parsed_position_snapshot out;
    const char* body =
        R"({"topic":"wallet","type":"snapshot","ts":1,"data":[{)"
        R"("accountType":"UNIFIED","coin":[)"
        R"({"coin":"USDT","walletBalance":"1000.5","equity":"1000.5"},)"
        R"({"coin":"BTC","walletBalance":"0.01"})"
        R"(]}]})";
    ASSERT_TRUE(p.parse_position_snapshot(body, out));
    ASSERT_EQ(out.balances.size(), 2u);
    EXPECT_EQ(out.balances[0].asset, "USDT");
    EXPECT_DOUBLE_EQ(out.balances[0].wallet_balance, 1000.5);
    EXPECT_EQ(out.balances[1].asset, "BTC");
}

TEST(BybitFuturesUserDataParser, OrderTopicNotPosition)
{
    BybitFuturesUserDataParser p;
    parsed_position_snapshot out;
    EXPECT_FALSE(p.parse_position_snapshot(order_push("New"), out));
}

TEST(BybitFuturesUserDataParser, SignedQtyHelper)
{
    EXPECT_DOUBLE_EQ(
        BybitFuturesUserDataParser::signed_position_qty("1.5", "Buy"), 1.5);
    EXPECT_DOUBLE_EQ(
        BybitFuturesUserDataParser::signed_position_qty("1.5", "Sell"), -1.5);
    EXPECT_DOUBLE_EQ(
        BybitFuturesUserDataParser::signed_position_qty("0", "None"), 0.0);
}

#endif // HAS_BYBIT
