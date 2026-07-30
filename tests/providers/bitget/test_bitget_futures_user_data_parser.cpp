#include <gtest/gtest.h>

#ifdef HAS_BITGET

#include "providers/bitget/bitget_futures_user_data_parser.h"
#include "providers/bitget/bitget_private_ws_transport.h"

#include <string>

namespace {

// UTA order-channel push (canned from plan §9.4 / Bitget docs shape).
std::string order_push(const std::string& status,
                       const std::string& cum = "0",
                       const std::string& side = "buy",
                       const std::string& client = "tt-1",
                       const std::string& order_id = "42")
{
    std::string j = R"({"action":"snapshot","arg":{"instType":"UTA","topic":"order"},"data":[{)";
    j += R"("category":"usdt-futures",)";
    j += R"("symbol":"BTCUSDT",)";
    j += R"("orderId":")" + order_id + R"(",)";
    j += R"("clientOid":")" + client + R"(",)";
    j += R"("side":")" + side + R"(",)";
    j += R"("orderType":"limit",)";
    j += R"("cumExecQty":")" + cum + R"(",)";
    j += R"("avgPrice":"60000",)";
    j += R"("orderStatus":")" + status + R"(",)";
    j += R"("cancelReason":"",)";
    j += R"("feeDetail":[{"feeCoin":"USDT","fee":"0.01"}],)";
    j += R"("updatedTime":"1700000000000")";
    j += R"(}],"ts":1700000000001})";
    return j;
}

std::string fill_push(const std::string& qty = "0.4",
                      const std::string& px = "60000",
                      const std::string& side = "buy",
                      const std::string& client = "tt-1",
                      const std::string& order_id = "42")
{
    std::string j = R"({"action":"snapshot","arg":{"instType":"UTA","topic":"fill"},"data":[{)";
    j += R"("symbol":"BTCUSDT",)";
    j += R"("orderId":")" + order_id + R"(",)";
    j += R"("clientOid":")" + client + R"(",)";
    j += R"("side":")" + side + R"(",)";
    j += R"("execQty":")" + qty + R"(",)";
    j += R"("execPrice":")" + px + R"(",)";
    j += R"("execTime":"1700000000002",)";
    j += R"("feeDetail":[{"feeCoin":"USDT","fee":"0.024"}],)";
    j += R"("category":"usdt-futures")";
    j += R"(}],"ts":1700000000003})";
    return j;
}

std::string position_push(const std::string& size,
                          const std::string& pos_side,
                          const std::string& margin = "crossed")
{
    std::string j = R"({"action":"snapshot","arg":{"instType":"UTA","topic":"position"},"data":[{)";
    j += R"("symbol":"BTCUSDT",)";
    j += R"("size":")" + size + R"(",)";
    j += R"("posSide":")" + pos_side + R"(",)";
    j += R"("marginMode":")" + margin + R"(",)";
    j += R"("holdMode":"one_way_mode",)";
    j += R"("updatedTime":"1700000000010")";
    j += R"(}],"ts":1700000000011})";
    return j;
}

} // namespace

// --- Pure transport helpers ---

TEST(BitgetPrivateWsTransportHelpers, LoginJsonExact)
{
    const std::string j = bitget::build_login_json(
        "key", "pass", "1627366780545", "SIG");
    EXPECT_EQ(
        j,
        R"({"op":"login","args":[{"apiKey":"key","passphrase":"pass","timestamp":"1627366780545","sign":"SIG"}]})");
}

TEST(BitgetPrivateWsTransportHelpers, LoginJsonEscapesSpecialChars)
{
    // Passphrase / key with JSON metacharacters must not break the frame.
    const std::string j = bitget::build_login_json(
        R"(k"ey)", R"(p\ass"x)", "1627366780545", "SI/G+=");
    EXPECT_EQ(
        j,
        R"({"op":"login","args":[{"apiKey":"k\"ey","passphrase":"p\\ass\"x","timestamp":"1627366780545","sign":"SI/G+="}]})");
}

TEST(BitgetPrivateWsTransportHelpers, AppendJsonEscapedControlChars)
{
    std::string out;
    bitget::append_json_escaped(out, "a\nb\tc");
    EXPECT_EQ(out, R"(a\nb\tc)");

    out.clear();
    bitget::append_json_escaped(out, std::string("\x01", 1));
    EXPECT_EQ(out, R"(\u0001)");
}

TEST(BitgetPrivateWsTransportHelpers, PrivateSubscribeExact)
{
    EXPECT_EQ(
        bitget::build_private_subscribe_json(),
        R"({"op":"subscribe","args":[{"instType":"UTA","topic":"order"},{"instType":"UTA","topic":"fill"},{"instType":"UTA","topic":"position"},{"instType":"UTA","topic":"account"}]})");
}

TEST(BitgetPrivateWsTransportHelpers, LoginSuccessCodes)
{
    EXPECT_TRUE(bitget::is_login_success(
        R"({"event":"login","code":0})"));
    EXPECT_TRUE(bitget::is_login_success(
        R"({"event":"login","code":"0"})"));
    EXPECT_TRUE(bitget::is_login_success(
        R"({"op":"login","code":"00000"})"));
    EXPECT_FALSE(bitget::is_login_success(
        R"({"event":"login","code":"30001"})"));
    EXPECT_FALSE(bitget::is_login_success(R"({"event":"subscribe"})"));
    EXPECT_TRUE(bitget::is_login_failure(
        R"({"event":"login","code":"40001","msg":"bad"})"));
}

TEST(BitgetPrivateWsTransport, ConstructDoesNotOpen)
{
    BitgetPrivateWsTransport tx("k", "s", "p");
    EXPECT_EQ(tx.state(), IFillTransport::lifecycle::closed);
}

TEST(BitgetPrivateWsTransport, OpenWithoutCredentialsErrors)
{
    BitgetPrivateWsTransport tx("", "", "");
    EXPECT_FALSE(tx.open());
    EXPECT_EQ(tx.state(), IFillTransport::lifecycle::error);
}

// --- orderStatus → kind (plan §9.4) ---

TEST(BitgetFuturesUserDataParser, OrderStatusNewIsAck)
{
    BitgetFuturesUserDataParser p;
    parsed_exec out;
    ASSERT_TRUE(p.parse(order_push("new"), out));
    EXPECT_EQ(out.k, parsed_exec::kind::ack);
    EXPECT_EQ(out.symbol, "BTCUSDT");
    EXPECT_EQ(out.client_order_id, "tt-1");
    EXPECT_EQ(out.exchange_order_id, "42");
    EXPECT_EQ(out.side, order_side::buy);
    EXPECT_DOUBLE_EQ(out.last_fill_qty, 0.0);
}

TEST(BitgetFuturesUserDataParser, OrderStatusLiveAndInitAreAck)
{
    BitgetFuturesUserDataParser p;
    parsed_exec out;
    ASSERT_TRUE(p.parse(order_push("live"), out));
    EXPECT_EQ(out.k, parsed_exec::kind::ack);
    ASSERT_TRUE(p.parse(order_push("init"), out));
    EXPECT_EQ(out.k, parsed_exec::kind::ack);
}

TEST(BitgetFuturesUserDataParser, OrderStatusPartiallyFilled)
{
    BitgetFuturesUserDataParser p;
    parsed_exec out;
    ASSERT_TRUE(p.parse(order_push("partially_filled", "0.4"), out));
    // Order channel leaves last_fill_qty=0 → demote to other (fill channel
    // owns incremental qty; avoids zero-qty partial_fill into the bridge).
    EXPECT_EQ(out.k, parsed_exec::kind::other);
    EXPECT_DOUBLE_EQ(out.cumulative_qty, 0.4);
    EXPECT_DOUBLE_EQ(out.last_fill_qty, 0.0);
}

TEST(BitgetFuturesUserDataParser, OrderStatusFilled)
{
    BitgetFuturesUserDataParser p;
    parsed_exec out;
    ASSERT_TRUE(p.parse(order_push("filled", "1.0"), out));
    // Order channel last_fill=0 → demote filled→other so bridge does not
    // untrack before fill-channel slices arrive (dual-channel race).
    EXPECT_EQ(out.k, parsed_exec::kind::other);
    EXPECT_DOUBLE_EQ(out.cumulative_qty, 1.0);
    EXPECT_DOUBLE_EQ(out.last_fill_qty, 0.0);
}

TEST(BitgetFuturesUserDataParser, OrderStatusCancelled)
{
    BitgetFuturesUserDataParser p;
    parsed_exec out;
    ASSERT_TRUE(p.parse(order_push("cancelled"), out));
    EXPECT_EQ(out.k, parsed_exec::kind::canceled);

    ASSERT_TRUE(p.parse(order_push("canceled"), out));
    EXPECT_EQ(out.k, parsed_exec::kind::canceled);
}

TEST(BitgetFuturesUserDataParser, OrderStatusRejected)
{
    BitgetFuturesUserDataParser p;
    parsed_exec out;
    std::string j = order_push("rejected");
    // inject cancelReason
    auto pos = j.find(R"("cancelReason":"")");
    ASSERT_NE(pos, std::string::npos);
    j.replace(pos, std::string(R"("cancelReason":"")").size(),
              R"("cancelReason":"INSUFFICIENT_MARGIN")");
    ASSERT_TRUE(p.parse(j, out));
    EXPECT_EQ(out.k, parsed_exec::kind::rejected);
    EXPECT_EQ(out.error, "INSUFFICIENT_MARGIN");
}

TEST(BitgetFuturesUserDataParser, OrderSellSide)
{
    BitgetFuturesUserDataParser p;
    parsed_exec out;
    ASSERT_TRUE(p.parse(order_push("new", "0", "sell"), out));
    EXPECT_EQ(out.side, order_side::sell);
}

TEST(BitgetFuturesUserDataParser, OrderFeeDetail)
{
    BitgetFuturesUserDataParser p;
    parsed_exec out;
    ASSERT_TRUE(p.parse(order_push("filled", "1"), out));
    EXPECT_DOUBLE_EQ(out.commission, 0.01);
    EXPECT_EQ(out.commission_asset, "USDT");
}

// --- fill channel ---

TEST(BitgetFuturesUserDataParser, FillChannelLastFill)
{
    BitgetFuturesUserDataParser p;
    parsed_exec out;
    ASSERT_TRUE(p.parse(fill_push("0.4", "60000.5"), out));
    EXPECT_EQ(out.k, parsed_exec::kind::partial_fill);
    EXPECT_DOUBLE_EQ(out.last_fill_qty, 0.4);
    EXPECT_DOUBLE_EQ(out.last_fill_price, 60000.5);
    EXPECT_EQ(out.client_order_id, "tt-1");
    EXPECT_EQ(out.exchange_order_id, "42");
    EXPECT_DOUBLE_EQ(out.commission, 0.024);
    EXPECT_EQ(out.commission_asset, "USDT");

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  out.ts.time_since_epoch())
                  .count();
    EXPECT_EQ(ms, 1700000000002LL);
}

TEST(BitgetFuturesUserDataParser, FillChannelSell)
{
    BitgetFuturesUserDataParser p;
    parsed_exec out;
    ASSERT_TRUE(p.parse(fill_push("0.1", "1", "sell"), out));
    EXPECT_EQ(out.side, order_side::sell);
}

// --- position channel (signed qty §9.5) ---

TEST(BitgetFuturesUserDataParser, PositionLongPositive)
{
    BitgetFuturesUserDataParser p;
    parsed_position_snapshot s;
    ASSERT_TRUE(p.parse_position_snapshot(position_push("0.5", "long"), s));
    ASSERT_EQ(s.positions.size(), 1u);
    EXPECT_EQ(s.positions[0].symbol, "BTCUSDT");
    EXPECT_DOUBLE_EQ(s.positions[0].qty, 0.5);
    EXPECT_EQ(s.positions[0].position_side, "long");
    EXPECT_EQ(s.positions[0].margin_type, "CROSSED");
    // Position topic → other (not order) so provider logs are not filtered.
    EXPECT_EQ(s.r, parsed_position_snapshot::reason::other);

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  s.ts.time_since_epoch())
                  .count();
    EXPECT_EQ(ms, 1700000000011LL);
}

TEST(BitgetFuturesUserDataParser, FillChannelDoesNotInventCumulative)
{
    BitgetFuturesUserDataParser p;
    parsed_exec out;
    ASSERT_TRUE(p.parse(fill_push("0.4", "60000.5"), out));
    EXPECT_EQ(out.k, parsed_exec::kind::partial_fill);
    EXPECT_DOUBLE_EQ(out.last_fill_qty, 0.4);
    // No cumExecQty on fill channel → leave 0 (bridge sums last_fill).
    EXPECT_DOUBLE_EQ(out.cumulative_qty, 0.0);
}

TEST(BitgetFuturesUserDataParser, PositionShortNegative)
{
    BitgetFuturesUserDataParser p;
    parsed_position_snapshot s;
    ASSERT_TRUE(p.parse_position_snapshot(
        position_push("1.25", "short", "isolated"), s));
    ASSERT_EQ(s.positions.size(), 1u);
    EXPECT_DOUBLE_EQ(s.positions[0].qty, -1.25);
    EXPECT_EQ(s.positions[0].margin_type, "ISOLATED");
}

TEST(BitgetFuturesUserDataParser, SignedPositionQtyHelper)
{
    EXPECT_DOUBLE_EQ(
        BitgetFuturesUserDataParser::signed_position_qty("2", "long"), 2.0);
    EXPECT_DOUBLE_EQ(
        BitgetFuturesUserDataParser::signed_position_qty("2", "short"), -2.0);
    EXPECT_DOUBLE_EQ(
        BitgetFuturesUserDataParser::signed_position_qty("-3", "short"), -3.0);
    EXPECT_DOUBLE_EQ(
        BitgetFuturesUserDataParser::signed_position_qty("-3", "long"), 3.0);
    EXPECT_DOUBLE_EQ(
        BitgetFuturesUserDataParser::signed_position_qty("-1.5", ""), -1.5);
}

TEST(BitgetFuturesUserDataParser, PositionMultiRows)
{
    BitgetFuturesUserDataParser p;
    parsed_position_snapshot s;
    std::string j =
        R"({"arg":{"instType":"UTA","topic":"position"},"data":[)"
        R"({"symbol":"BTCUSDT","size":"1","posSide":"long","marginMode":"crossed"},)"
        R"({"symbol":"ETHUSDT","size":"2","posSide":"short","marginMode":"isolated"}],"ts":1})";
    ASSERT_TRUE(p.parse_position_snapshot(j, s));
    ASSERT_EQ(s.positions.size(), 2u);
    EXPECT_DOUBLE_EQ(s.positions[0].qty, 1.0);
    EXPECT_EQ(s.positions[1].symbol, "ETHUSDT");
    EXPECT_DOUBLE_EQ(s.positions[1].qty, -2.0);
}

// --- routing / rejects ---

TEST(BitgetFuturesUserDataParser, ParseRejectsPositionTopic)
{
    BitgetFuturesUserDataParser p;
    parsed_exec out;
    EXPECT_FALSE(p.parse(position_push("1", "long"), out));
}

TEST(BitgetFuturesUserDataParser, ParsePositionSnapshotRejectsOrderTopic)
{
    BitgetFuturesUserDataParser p;
    parsed_position_snapshot s;
    EXPECT_FALSE(p.parse_position_snapshot(order_push("new"), s));
}

TEST(BitgetFuturesUserDataParser, ParsePositionSnapshotRejectsFillTopic)
{
    BitgetFuturesUserDataParser p;
    parsed_position_snapshot s;
    EXPECT_FALSE(p.parse_position_snapshot(fill_push(), s));
}

TEST(BitgetFuturesUserDataParser, ClassifyOrderStatusTable)
{
    using K = parsed_exec::kind;
    EXPECT_EQ(BitgetFuturesUserDataParser::classify_order_status("new"), K::ack);
    EXPECT_EQ(BitgetFuturesUserDataParser::classify_order_status("live"), K::ack);
    EXPECT_EQ(BitgetFuturesUserDataParser::classify_order_status("init"), K::ack);
    EXPECT_EQ(BitgetFuturesUserDataParser::classify_order_status("partially_filled"),
              K::partial_fill);
    EXPECT_EQ(BitgetFuturesUserDataParser::classify_order_status("filled"),
              K::full_fill);
    EXPECT_EQ(BitgetFuturesUserDataParser::classify_order_status("cancelled"),
              K::canceled);
    EXPECT_EQ(BitgetFuturesUserDataParser::classify_order_status("canceled"),
              K::canceled);
    EXPECT_EQ(BitgetFuturesUserDataParser::classify_order_status("rejected"),
              K::rejected);
    EXPECT_EQ(BitgetFuturesUserDataParser::classify_order_status("weird"),
              K::other);
}

TEST(BitgetFuturesUserDataParser, OrderTsFromUpdatedTime)
{
    BitgetFuturesUserDataParser p;
    parsed_exec out;
    ASSERT_TRUE(p.parse(order_push("new"), out));
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  out.ts.time_since_epoch())
                  .count();
    EXPECT_EQ(ms, 1700000000000LL);
}

#endif // HAS_BITGET
