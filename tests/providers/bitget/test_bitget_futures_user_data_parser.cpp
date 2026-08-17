#include <gtest/gtest.h>

#ifdef HAS_BITGET

#include "providers/bitget/bitget_futures_user_data_parser.h"
#include "providers/bitget/bitget_private_ws_transport.h"

#include <string>
#include <memory>
#include <thread>

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
    EXPECT_FALSE(bitget::is_login_success(
        R"(garbage {"event":"login","code":"0"})"));
    EXPECT_FALSE(bitget::is_login_success(
        R"({"event":"login","code":"0","code":"30001"})"));
    EXPECT_FALSE(bitget::is_login_success(
        R"({"nested":{"event":"login","code":"0"}})"));
    EXPECT_FALSE(bitget::is_login_success(
        R"({"event":"login","op":"login","code":"0"})"));
}

TEST(BitgetPrivateWsTransportHelpers, SubscriptionAckIsAuthoritativeAndTopicBound)
{
    std::string_view topic;
    EXPECT_EQ(bitget::classify_private_subscription_ack(
                  R"({"event":"subscribe","arg":{"instType":"UTA","topic":"order"},"connId":"x"})",
                  topic),
              bitget::subscription_ack::accepted);
    EXPECT_EQ(topic, "order");

    EXPECT_EQ(bitget::classify_private_subscription_ack(
                  R"({"event":"subscribe","arg":{"instType":"UTA","topic":"fill"},"code":"0"})",
                  topic),
              bitget::subscription_ack::accepted);
    EXPECT_EQ(topic, "fill");

    EXPECT_EQ(bitget::classify_private_subscription_ack(
                  R"({"event":"error","arg":{"instType":"UTA","topic":"order"},"code":"30001"})",
                  topic),
              bitget::subscription_ack::rejected);
    EXPECT_EQ(bitget::classify_private_subscription_ack(
                  R"({"event":"subscribe","arg":{"instType":"UTA","topic":"order"},"code":"0","code":"1"})",
                  topic),
              bitget::subscription_ack::rejected);
    EXPECT_EQ(bitget::classify_private_subscription_ack(
                  R"({"event":"subscribe","arg":{"instType":"UTA","topic":"unknown"}})",
                  topic),
              bitget::subscription_ack::rejected);
    EXPECT_EQ(bitget::classify_private_subscription_ack(
                  R"({"event":"subscribe","nested":{"arg":{"instType":"UTA","topic":"order"}}})",
                  topic),
              bitget::subscription_ack::rejected);
    EXPECT_EQ(bitget::classify_private_subscription_ack(
                  R"({"event":"subscribe","arg":{"instType":"UTA","topic":"order"}} trailing)",
                  topic),
              bitget::subscription_ack::unrelated);
}

TEST(BitgetPrivateWsTransportHelpers, SubscriptionTrackerRequiresAllFourTopics)
{
    bitget::private_subscription_tracker tracker;
    EXPECT_EQ(tracker.consume(
                  R"({"event":"subscribe","arg":{"instType":"UTA","topic":"fill"}})"),
              bitget::subscription_progress::waiting);
    EXPECT_EQ(tracker.consume(
                  R"({"event":"subscribe","arg":{"instType":"UTA","topic":"order"}})"),
              bitget::subscription_progress::waiting);
    EXPECT_EQ(tracker.consume(
                  R"({"event":"subscribe","arg":{"instType":"UTA","topic":"fill"}})"),
              bitget::subscription_progress::waiting)
        << "a duplicate ACK must not advance readiness";
    EXPECT_EQ(tracker.consume(
                  R"({"event":"subscribe","arg":{"instType":"UTA","topic":"account"}})"),
              bitget::subscription_progress::waiting);
    EXPECT_EQ(tracker.consume(
                  R"({"event":"subscribe","arg":{"instType":"UTA","topic":"position"}})"),
              bitget::subscription_progress::ready);
}

TEST(BitgetPrivateWsTransportHelpers, SubscriptionTrackerRejectsAndIgnoresSafely)
{
    bitget::private_subscription_tracker tracker;
    EXPECT_EQ(tracker.consume(R"({"event":"info","msg":"connected"})"),
              bitget::subscription_progress::waiting);
    EXPECT_EQ(tracker.consume(
                  R"({"event":"subscribe","nested":{"arg":{"instType":"UTA","topic":"order"}}})"),
              bitget::subscription_progress::rejected);

    bitget::private_subscription_tracker rejection;
    EXPECT_EQ(rejection.consume(
                  R"({"event":"error","code":"30001","msg":"denied"})"),
              bitget::subscription_progress::rejected);
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

TEST(BitgetPrivateWsTransport, InitialSocketFailureRefusesAndCleansUp)
{
    net::io_context server_ioc;
    tcp::acceptor acceptor(server_ioc,
        tcp::endpoint(net::ip::address_v4::loopback(), 0));
    auto socket = std::make_shared<tcp::socket>(server_ioc);
    acceptor.async_accept(*socket, [socket](beast::error_code) {
        beast::error_code ignored;
        socket->close(ignored);
    });
    std::thread server([&] { server_ioc.run(); });

    BitgetPrivateWsTransport tx(
        "key", "secret", "pass", "127.0.0.1",
        std::to_string(acceptor.local_endpoint().port()), "/v3/ws/private");
    const auto started = std::chrono::steady_clock::now();
    EXPECT_FALSE(tx.open());
    EXPECT_LT(std::chrono::steady_clock::now() - started,
              std::chrono::seconds(2));
    EXPECT_EQ(tx.state(), IFillTransport::lifecycle::closed);

    beast::error_code ignored;
    acceptor.close(ignored);
    server_ioc.stop();
    server.join();
}

// --- orderStatus → kind (plan §9.4) ---

TEST(BitgetFuturesUserDataParser, OrderStatusNewIsAck)
{
    BitgetFuturesUserDataParser p;
    parsed_exec out;
    ASSERT_EQ(p.parse(order_push("new"), out), execution_parse_result::valid);
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
    ASSERT_EQ(p.parse(order_push("live"), out), execution_parse_result::valid);
    EXPECT_EQ(out.k, parsed_exec::kind::ack);
    ASSERT_EQ(p.parse(order_push("init"), out), execution_parse_result::valid);
    EXPECT_EQ(out.k, parsed_exec::kind::ack);
}

TEST(BitgetFuturesUserDataParser, OrderStatusPartiallyFilled)
{
    BitgetFuturesUserDataParser p;
    parsed_exec out;
    ASSERT_EQ(p.parse(order_push("partially_filled", "0.4"), out),
              execution_parse_result::valid);
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
    ASSERT_EQ(p.parse(order_push("filled", "1.0"), out),
              execution_parse_result::valid);
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
    ASSERT_EQ(p.parse(order_push("cancelled"), out),
              execution_parse_result::valid);
    EXPECT_EQ(out.k, parsed_exec::kind::canceled);

    ASSERT_EQ(p.parse(order_push("canceled"), out),
              execution_parse_result::valid);
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
    ASSERT_EQ(p.parse(j, out), execution_parse_result::valid);
    EXPECT_EQ(out.k, parsed_exec::kind::rejected);
    EXPECT_EQ(out.error, "INSUFFICIENT_MARGIN");
}

TEST(BitgetFuturesUserDataParser, OrderSellSide)
{
    BitgetFuturesUserDataParser p;
    parsed_exec out;
    ASSERT_EQ(p.parse(order_push("new", "0", "sell"), out),
              execution_parse_result::valid);
    EXPECT_EQ(out.side, order_side::sell);
}

TEST(BitgetFuturesUserDataParser, OrderFeeDetail)
{
    BitgetFuturesUserDataParser p;
    parsed_exec out;
    ASSERT_EQ(p.parse(order_push("filled", "1"), out),
              execution_parse_result::valid);
    EXPECT_DOUBLE_EQ(out.commission, 0.01);
    EXPECT_EQ(out.commission_asset, "USDT");
}

// --- fill channel ---

TEST(BitgetFuturesUserDataParser, FillChannelLastFill)
{
    BitgetFuturesUserDataParser p;
    parsed_exec out;
    ASSERT_EQ(p.parse(fill_push("0.4", "60000.5"), out),
              execution_parse_result::valid);
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
    ASSERT_EQ(p.parse(fill_push("0.1", "1", "sell"), out),
              execution_parse_result::valid);
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
    ASSERT_EQ(p.parse(fill_push("0.4", "60000.5"), out),
              execution_parse_result::valid);
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
    EXPECT_EQ(p.parse(position_push("1", "long"), out),
              execution_parse_result::unrelated);
}

TEST(BitgetFuturesUserDataParser, HarmlessControlsLeaveOutputUntouched)
{
    BitgetFuturesUserDataParser p;
    parsed_exec out;
    out.symbol = "sentinel";
    out.client_order_id = "keep";

    EXPECT_EQ(p.parse("ping", out), execution_parse_result::unrelated);
    EXPECT_EQ(p.parse("pong", out), execution_parse_result::unrelated);
    EXPECT_EQ(p.parse(
                  R"({"event":"subscribe","arg":{"instType":"UTA","topic":"order"},"code":"0"})",
                  out),
              execution_parse_result::unrelated);
    EXPECT_EQ(p.parse(R"({"event":"info","msg":"connected"})", out),
              execution_parse_result::unrelated);
    EXPECT_EQ(out.symbol, "sentinel");
    EXPECT_EQ(out.client_order_id, "keep");
}

TEST(BitgetFuturesUserDataParser,
     MalformedExecutionOrErrorFailsClosedAndPreservesOutput)
{
    BitgetFuturesUserDataParser p;
    parsed_exec out;
    out.symbol = "sentinel";
    out.client_order_id = "keep";

    EXPECT_EQ(p.parse(R"({"event":"error","code":"30001"})", out),
              execution_parse_result::malformed);
    EXPECT_EQ(p.parse(
                  R"({"event":"subscribe","arg":{"instType":"UTA","topic":"order"},"code":"30001"})",
                  out),
              execution_parse_result::malformed);
    EXPECT_EQ(p.parse(
                  R"({"event":"subscribe","arg":{"instType":"UTA","topic":"wrong"},"code":"0"})",
                  out),
              execution_parse_result::malformed);
    // A control discriminator must not hide an execution-shaped push with a
    // missing data member.  A genuine subscribe ACK has no action field.
    EXPECT_EQ(p.parse(
                  R"({"event":"info","action":"snapshot","arg":{"instType":"UTA","topic":"order"},"ts":1})",
                  out),
              execution_parse_result::malformed);
    EXPECT_EQ(p.parse(R"({"arg":{"instType":"UTA","topic":"order"},"data":[{}],"ts":1})", out),
              execution_parse_result::malformed);
    EXPECT_EQ(p.parse(
                  R"({"action":"snapshot","data":[{"category":"usdt-futures","symbol":"BTCUSDT","orderId":"42","clientOid":"tt-1","side":"buy","execQty":"1","execPrice":"100","execTime":"1700000000002","fee":"0","feeCoin":"USDT"}],"ts":1700000000003})",
                  out),
              execution_parse_result::malformed);
    EXPECT_EQ(p.parse(
                  R"({"action":"snapshot","arg":{"instType":"UTA","topic":"unknown"},"data":[{"category":"usdt-futures","symbol":"BTCUSDT","orderId":"42","clientOid":"tt-1","side":"buy","execQty":"1","execPrice":"100","execTime":"1700000000002","fee":"0","feeCoin":"USDT"}],"ts":1700000000003})",
                  out),
              execution_parse_result::malformed);

    auto execution_on_account_channel = fill_push();
    const auto fill_topic = execution_on_account_channel.find(R"("topic":"fill")");
    ASSERT_NE(fill_topic, std::string::npos);
    execution_on_account_channel.replace(
        fill_topic, std::string(R"("topic":"fill")").size(),
        R"("topic":"account")");
    EXPECT_EQ(p.parse(execution_on_account_channel, out),
              execution_parse_result::malformed);

    EXPECT_EQ(p.parse(
                  R"({"action":"snapshot","arg":{"instType":"UTA","topic":"position"},"data":{"category":"usdt-futures","symbol":"BTCUSDT","orderId":"42","clientOid":"tt-1","side":"buy","execQty":"1","execPrice":"100"},"ts":1700000000003})",
                  out),
              execution_parse_result::malformed);
    EXPECT_EQ(p.parse(fill_push("0.4", "60000", "garbage"), out),
              execution_parse_result::malformed);

    auto bad_qty = fill_push();
    const auto qty = bad_qty.find(R"("execQty":"0.4")");
    ASSERT_NE(qty, std::string::npos);
    bad_qty.replace(qty, std::string(R"("execQty":"0.4")").size(),
                    R"("execQty":"nan")");
    EXPECT_EQ(p.parse(bad_qty, out), execution_parse_result::malformed);

    auto bad_fee = fill_push();
    const auto fee = bad_fee.find(R"("fee":"0.024")");
    ASSERT_NE(fee, std::string::npos);
    bad_fee.replace(fee, std::string(R"("fee":"0.024")").size(),
                    R"("fee":"nan")");
    EXPECT_EQ(p.parse(bad_fee, out), execution_parse_result::malformed);

    auto embedded_fill = fill_push();
    embedded_fill.insert(1, R"("event":"info",)");
    EXPECT_EQ(p.parse(embedded_fill, out), execution_parse_result::malformed);

    auto wrong_surface = fill_push();
    const auto uta = wrong_surface.find(R"("instType":"UTA")");
    ASSERT_NE(uta, std::string::npos);
    wrong_surface.replace(uta, std::string(R"("instType":"UTA")").size(),
                          R"("instType":"SPOT")");
    EXPECT_EQ(p.parse(wrong_surface, out), execution_parse_result::malformed);

    auto wrong_category = fill_push();
    const auto category = wrong_category.find(R"("category":"usdt-futures")");
    ASSERT_NE(category, std::string::npos);
    wrong_category.replace(category,
                           std::string(R"("category":"usdt-futures")").size(),
                           R"("category":"spot")");
    EXPECT_EQ(p.parse(wrong_category, out), execution_parse_result::malformed);

    auto wrong_action = fill_push();
    const auto action = wrong_action.find(R"("action":"snapshot")");
    ASSERT_NE(action, std::string::npos);
    wrong_action.replace(action, std::string(R"("action":"snapshot")").size(),
                         R"("action":"update")");
    EXPECT_EQ(p.parse(wrong_action, out), execution_parse_result::malformed);

    auto missing_action = order_push("new");
    const auto action_prefix = missing_action.find(R"("action":"snapshot",)");
    ASSERT_NE(action_prefix, std::string::npos);
    missing_action.erase(action_prefix,
                         std::string(R"("action":"snapshot",)").size());
    EXPECT_EQ(p.parse(missing_action, out), execution_parse_result::malformed);

    auto duplicate_action = fill_push();
    duplicate_action.insert(1, R"("action":"snapshot",)");
    EXPECT_EQ(p.parse(duplicate_action, out), execution_parse_result::malformed);

    auto missing_order_id = fill_push();
    const auto order_id = missing_order_id.find(R"("orderId":"42",)");
    ASSERT_NE(order_id, std::string::npos);
    missing_order_id.erase(order_id, std::string(R"("orderId":"42",)").size());
    EXPECT_EQ(p.parse(missing_order_id, out), execution_parse_result::malformed);

    auto untyped_fee = fill_push();
    const auto fee_coin = untyped_fee.find(R"("feeCoin":"USDT")");
    ASSERT_NE(fee_coin, std::string::npos);
    untyped_fee.replace(fee_coin, std::string(R"("feeCoin":"USDT")").size(),
                        R"("feeCoin":"")");
    EXPECT_EQ(p.parse(untyped_fee, out), execution_parse_result::malformed);

    auto empty_time = fill_push();
    const auto time = empty_time.find(R"("execTime":"1700000000002")");
    ASSERT_NE(time, std::string::npos);
    empty_time.replace(time,
                       std::string(R"("execTime":"1700000000002")").size(),
                       R"("execTime":"")");
    EXPECT_EQ(p.parse(empty_time, out), execution_parse_result::malformed);

    auto overflow_time = fill_push();
    const auto timestamp = overflow_time.find("1700000000003");
    ASSERT_NE(timestamp, std::string::npos);
    overflow_time.replace(timestamp, std::string("1700000000003").size(),
                          "9223372036854775807");
    EXPECT_EQ(p.parse(overflow_time, out), execution_parse_result::malformed);
    EXPECT_EQ(out.symbol, "sentinel");
    EXPECT_EQ(out.client_order_id, "keep");
}

TEST(BitgetFuturesUserDataParser, FundingParserDoesNotPreemptOrderText)
{
    BitgetFuturesUserDataParser p;
    parsed_funding_update funding;
    const auto order = order_push("new", "0", "buy", "fund-client");

    EXPECT_EQ(p.parse_funding_update(order, funding),
              funding_parse_result::not_funding);
}

TEST(BitgetFuturesUserDataParser, FundingTimestampMustFitSystemClock)
{
    BitgetFuturesUserDataParser p;
    parsed_funding_update funding;
    const std::string overflow =
        R"({"action":"snapshot","arg":{"instType":"UTA","topic":"account"},"data":[{"bizType":"funding_fee","coin":"USDT","balanceChange":"-0.5"}],"ts":"9223372036854775807"})";

    EXPECT_EQ(p.parse_funding_update(overflow, funding),
              funding_parse_result::invalid);
}

TEST(BitgetFuturesUserDataParser, FundingRequiresAuthoritativeUtaAccountRoute)
{
    BitgetFuturesUserDataParser p;
    parsed_funding_update funding;
    constexpr std::string_view body =
        R"({"arg":{"topic":"account"},"data":[{"bizType":"funding_fee","coin":"USDT","balanceChange":"-0.5"}],"ts":1700000000000})";
    EXPECT_EQ(p.parse_funding_update(body, funding),
              funding_parse_result::invalid);

    constexpr std::string_view wrong_surface =
        R"({"arg":{"instType":"SPOT","topic":"account"},"data":[{"bizType":"funding_fee","coin":"USDT","balanceChange":"-0.5"}],"ts":1700000000000})";
    EXPECT_EQ(p.parse_funding_update(wrong_surface, funding),
              funding_parse_result::invalid);

    constexpr std::string_view contradictory_control =
        R"({"event":"info","arg":{"instType":"UTA","topic":"account"},"data":[{"bizType":"funding_fee","coin":"USDT","balanceChange":"-0.5"}],"ts":1700000000000})";
    EXPECT_EQ(p.parse_funding_update(contradictory_control, funding),
              funding_parse_result::not_funding);
    parsed_exec execution;
    EXPECT_EQ(p.parse(contradictory_control, execution),
              execution_parse_result::malformed);
}

TEST(BitgetFuturesUserDataParser, FilledOrderRequiresPositiveCumulativeQuantity)
{
    BitgetFuturesUserDataParser p;
    parsed_exec out;
    EXPECT_EQ(p.parse(order_push("partially_filled", "0"), out),
              execution_parse_result::malformed);
    EXPECT_EQ(p.parse(order_push("filled", "0"), out),
              execution_parse_result::malformed);
}

TEST(BitgetFuturesUserDataParser,
     AcknowledgedOrRejectedOrderCannotClaimExecutedQuantity)
{
    BitgetFuturesUserDataParser p;
    parsed_exec out;
    for (const auto status : {"new", "live", "init", "rejected"})
    {
        EXPECT_EQ(p.parse(order_push(status, "1"), out),
                  execution_parse_result::malformed)
            << status;
    }
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
    ASSERT_EQ(p.parse(order_push("new"), out), execution_parse_result::valid);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  out.ts.time_since_epoch())
                  .count();
    EXPECT_EQ(ms, 1700000000000LL);
}

#endif // HAS_BITGET
