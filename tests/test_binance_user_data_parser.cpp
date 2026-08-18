#include <gtest/gtest.h>

#include "providers/binance/binance_user_data_parser.h"

#include <string>

namespace {

// Minimal executionReport-shaped payload. Binance's real format uses many more
// fields - the parser only needs these.
std::string report(const std::string& x, const std::string& X,
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
    std::string j = R"({"e":"executionReport",)";
    j += R"("E":1700000000000,)";
    j += R"("s":"BTCUSDT",)";
    j += R"("c":")" + c + R"(",)";
    j += R"("S":")" + S + R"(",)";
    j += R"("x":")" + x + R"(",)";
    j += R"("X":")" + X + R"(",)";
    j += R"("r":")" + r + R"(",)";
    j += R"("i":)" + i + ",";
    j += R"("I":9001,)";
    j += R"("l":")" + l + R"(",)";
    j += R"("L":")" + L + R"(",)";
    j += R"("z":")" + z + R"(",)";
    j += R"("n":")" + n + R"(",)";
    j += R"("N":")" + N + R"("})";
    return j;
}

}

TEST(BinanceUserDataParser, RejectsNonExecutionReports)
{
    BinanceUserDataParser p;
    parsed_exec out;

    std::string bal = R"({"e":"outboundAccountPosition","E":1})";
    EXPECT_EQ(p.parse(bal, out), execution_parse_result::unrelated);

    std::string listen = R"({"e":"listenKeyExpired","E":1})";
    EXPECT_EQ(p.parse(listen, out), execution_parse_result::malformed);

    std::string terminated = R"({"e":"eventStreamTerminated","E":1})";
    EXPECT_EQ(p.parse(terminated, out), execution_parse_result::malformed);

    std::string list_status = R"({"e":"listStatus","E":1,"l":"ALL_DONE","L":"ALL_DONE"})";
    EXPECT_EQ(p.parse(list_status, out), execution_parse_result::malformed);
}

TEST(BinanceUserDataParser, HarmlessControlsLeaveOutputUntouched)
{
    BinanceUserDataParser p;
    parsed_exec out;
    out.symbol = "sentinel";
    out.client_order_id = "keep";

    EXPECT_EQ(p.parse("ping", out), execution_parse_result::unrelated);
    EXPECT_EQ(p.parse("pong", out), execution_parse_result::unrelated);
    EXPECT_EQ(p.parse(R"({"result":null,"id":1})", out),
              execution_parse_result::unrelated);
    EXPECT_TRUE(p.is_harmless_private_control("ping"));
    EXPECT_TRUE(p.is_harmless_private_control("pong"));
    EXPECT_FALSE(p.is_harmless_private_control(
        R"({"result":null,"id":1})"));
    EXPECT_FALSE(p.is_harmless_private_control(" ping"));
    EXPECT_EQ(out.symbol, "sentinel");
    EXPECT_EQ(out.client_order_id, "keep");
}

TEST(BinanceUserDataParser, MalformedKnownEnvelopeFailsClosedAndPreservesOutput)
{
    BinanceUserDataParser p;
    parsed_exec out;
    out.symbol = "sentinel";
    out.client_order_id = "keep";

    EXPECT_EQ(p.parse(R"({"e":"executionReport")", out),
              execution_parse_result::malformed);
    EXPECT_EQ(p.parse(R"({"e":"executionReport","e":"executionReport"})", out),
              execution_parse_result::malformed);
    EXPECT_EQ(p.parse(R"({"s":"BTCUSDT","c":"tt-1","S":"BUY","x":"NEW","X":"NEW","i":42})", out),
              execution_parse_result::malformed);
    EXPECT_EQ(p.parse(R"({"e":"bogus","s":"BTCUSDT","c":"tt-1","S":"BUY","x":"NEW","X":"NEW","i":42})", out),
              execution_parse_result::malformed);
    EXPECT_EQ(p.parse(report("NEW", "FILLED"), out),
              execution_parse_result::malformed);
    EXPECT_EQ(p.parse(report("TRADE", "FILLED", "nan", "100", "1"), out),
              execution_parse_result::malformed);
    EXPECT_EQ(p.parse(report("NEW", "NEW", "0", "0", "0", "tt-1", "HOLD"), out),
              execution_parse_result::malformed);
    auto missing_fee = report("TRADE", "FILLED", "1", "100", "1");
    const auto fee = missing_fee.find(R"(,"n":"0.0")");
    ASSERT_NE(fee, std::string::npos);
    missing_fee.erase(fee, std::string(R"(,"n":"0.0")").size());
    EXPECT_EQ(p.parse(missing_fee, out), execution_parse_result::malformed);
    auto missing_order_id = report("NEW", "NEW");
    const auto order_id = missing_order_id.find(R"("i":42,)");
    ASSERT_NE(order_id, std::string::npos);
    missing_order_id.erase(order_id, std::string(R"("i":42,)").size());
    EXPECT_EQ(p.parse(missing_order_id, out), execution_parse_result::malformed);
    auto nonnumeric_order_id = report("NEW", "NEW");
    const auto numeric_order_id = nonnumeric_order_id.find(R"("i":42)");
    ASSERT_NE(numeric_order_id, std::string::npos);
    nonnumeric_order_id.replace(numeric_order_id, std::string(R"("i":42)").size(),
                                R"("i":"alpha-7")");
    EXPECT_EQ(p.parse(nonnumeric_order_id, out), execution_parse_result::malformed);
    auto empty_fee = report("NEW", "NEW");
    const auto empty = empty_fee.find(R"("n":"0.0")");
    ASSERT_NE(empty, std::string::npos);
    empty_fee.replace(empty, std::string(R"("n":"0.0")").size(),
                      R"("n":"")");
    EXPECT_EQ(p.parse(empty_fee, out), execution_parse_result::malformed);
    auto overflow_ts = report("NEW", "NEW");
    const auto timestamp = overflow_ts.find("1700000000000");
    ASSERT_NE(timestamp, std::string::npos);
    overflow_ts.replace(timestamp, std::string("1700000000000").size(),
                        "9223372036854775807");
    EXPECT_EQ(p.parse(overflow_ts, out), execution_parse_result::malformed);
    EXPECT_EQ(out.symbol, "sentinel");
    EXPECT_EQ(out.client_order_id, "keep");
}

TEST(BinanceUserDataParser, NullableCommissionAssetIsAccepted)
{
    BinanceUserDataParser p;
    parsed_exec out;
    auto j = report("NEW", "NEW");
    const auto field = j.find(R"("N":"USDT")");
    ASSERT_NE(field, std::string::npos);
    j.replace(field, std::string(R"("N":"USDT")").size(), R"("N":null)");

    ASSERT_EQ(p.parse(j, out), execution_parse_result::valid);
    EXPECT_TRUE(out.commission_asset.empty());
}

TEST(BinanceUserDataParser, NonzeroCommissionRequiresAsset)
{
    BinanceUserDataParser p;
    parsed_exec out;
    auto j = report("TRADE", "FILLED", "1", "100", "1", "tt-1",
                    "BUY", "42", "0.01", "USDT");
    const auto field = j.find(R"("N":"USDT")");
    ASSERT_NE(field, std::string::npos);
    j.replace(field, std::string(R"("N":"USDT")").size(), R"("N":null)");

    EXPECT_EQ(p.parse(j, out), execution_parse_result::malformed);
}

TEST(BinanceUserDataParser, NewAck)
{
    BinanceUserDataParser p;
    parsed_exec out;
    auto j = report("NEW", "NEW");

    ASSERT_EQ(p.parse(j, out), execution_parse_result::valid);
    EXPECT_EQ(out.k, parsed_exec::kind::ack);
    EXPECT_EQ(out.symbol, "BTCUSDT");
    EXPECT_EQ(out.client_order_id, "tt-1");
    EXPECT_EQ(out.exchange_order_id, "42");
    EXPECT_EQ(out.side, order_side::buy);
    EXPECT_DOUBLE_EQ(out.last_fill_qty, 0.0);
    EXPECT_TRUE(out.has_cumulative_qty);
    EXPECT_TRUE(out.execution_id.empty());
}

TEST(BinanceUserDataParser, PartialTrade)
{
    BinanceUserDataParser p;
    parsed_exec out;
    auto j = report("TRADE", "PARTIALLY_FILLED",
                    "0.4", "60000.0", "0.4");

    ASSERT_EQ(p.parse(j, out), execution_parse_result::valid);
    EXPECT_EQ(out.k, parsed_exec::kind::partial_fill);
    EXPECT_DOUBLE_EQ(out.last_fill_qty, 0.4);
    EXPECT_DOUBLE_EQ(out.last_fill_price, 60000.0);
    EXPECT_DOUBLE_EQ(out.cumulative_qty, 0.4);
    EXPECT_TRUE(out.has_cumulative_qty);
    EXPECT_EQ(out.execution_id, "9001");
}

TEST(BinanceUserDataParser, FullTrade)
{
    BinanceUserDataParser p;
    parsed_exec out;
    auto j = report("TRADE", "FILLED",
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
    EXPECT_TRUE(out.has_cumulative_qty);
    EXPECT_EQ(out.execution_id, "9001");
}

TEST(BinanceUserDataParser,
     LifecycleReportsRejectIncrementalEconomicsButAllowCumulativeProof)
{
    BinanceUserDataParser p;
    parsed_exec out;
    const auto expect_incremental_economics_rejected =
        [&](const std::string& execution_type, const std::string& status) {
            EXPECT_EQ(p.parse(report(execution_type, status,
                                     "1", "0", "0"), out),
                      execution_parse_result::malformed);
            EXPECT_EQ(p.parse(report(execution_type, status,
                                     "0", "100", "0"), out),
                      execution_parse_result::malformed);
            EXPECT_EQ(p.parse(report(execution_type, status,
                                     "0", "0", "0", "tt-1", "BUY",
                                     "42", "0.01"), out),
                      execution_parse_result::malformed);
        };

    expect_incremental_economics_rejected("NEW", "NEW");
    expect_incremental_economics_rejected("CANCELED", "CANCELED");
    expect_incremental_economics_rejected("REJECTED", "REJECTED");
    expect_incremental_economics_rejected("EXPIRED", "EXPIRED");

    ASSERT_EQ(p.parse(report("CANCELED", "CANCELED", "0", "0", "0.4"), out),
              execution_parse_result::valid);
    EXPECT_EQ(out.k, parsed_exec::kind::canceled);
    EXPECT_TRUE(out.has_cumulative_qty);
    EXPECT_DOUBLE_EQ(out.cumulative_qty, 0.4);
}

TEST(BinanceUserDataParser, EconomicFillsRequireCanonicalExecutionId)
{
    BinanceUserDataParser p;
    parsed_exec out;

    auto missing = report("TRADE", "FILLED", "1", "100", "1");
    const auto missing_field = missing.find(R"(,"I":9001)");
    ASSERT_NE(missing_field, std::string::npos);
    missing.erase(missing_field, std::string(R"(,"I":9001)").size());
    EXPECT_EQ(p.parse(missing, out), execution_parse_result::malformed);

    auto zero = report("TRADE", "FILLED", "1", "100", "1");
    const auto execution_field = zero.find(R"("I":9001)");
    ASSERT_NE(execution_field, std::string::npos);
    zero.replace(execution_field, std::string(R"("I":9001)").size(),
                 R"("I":0)");
    EXPECT_EQ(p.parse(zero, out), execution_parse_result::malformed);

    auto negative = report("TRADE", "FILLED", "1", "100", "1");
    const auto negative_field = negative.find(R"("I":9001)");
    ASSERT_NE(negative_field, std::string::npos);
    negative.replace(negative_field, std::string(R"("I":9001)").size(),
                     R"("I":-1)");
    EXPECT_EQ(p.parse(negative, out), execution_parse_result::malformed);

    auto nonnumeric = report("TRADE", "FILLED", "1", "100", "1");
    const auto nonnumeric_field = nonnumeric.find(R"("I":9001)");
    ASSERT_NE(nonnumeric_field, std::string::npos);
    nonnumeric.replace(nonnumeric_field, std::string(R"("I":9001)").size(),
                       R"("I":"not-an-id")");
    EXPECT_EQ(p.parse(nonnumeric, out), execution_parse_result::malformed);

    auto numeric_string = report("TRADE", "FILLED", "1", "100", "1");
    const auto string_field = numeric_string.find(R"("I":9001)");
    ASSERT_NE(string_field, std::string::npos);
    numeric_string.replace(string_field, std::string(R"("I":9001)").size(),
                           R"("I":"0009001")");
    ASSERT_EQ(p.parse(numeric_string, out), execution_parse_result::valid);
    EXPECT_EQ(out.execution_id, "9001");
}

TEST(BinanceUserDataParser, LifecycleReportsDoNotRequireExecutionId)
{
    BinanceUserDataParser p;
    parsed_exec out;

    auto ack = report("NEW", "NEW");
    const auto execution_field = ack.find(R"(,"I":9001)");
    ASSERT_NE(execution_field, std::string::npos);
    ack.erase(execution_field, std::string(R"(,"I":9001)").size());
    ASSERT_EQ(p.parse(ack, out), execution_parse_result::valid);
    EXPECT_EQ(out.k, parsed_exec::kind::ack);
    EXPECT_TRUE(out.execution_id.empty());
    EXPECT_TRUE(out.has_cumulative_qty);

    auto no_cumulative = ack;
    const auto cumulative_field = no_cumulative.find(R"(,"z":"0.0")");
    ASSERT_NE(cumulative_field, std::string::npos);
    no_cumulative.erase(cumulative_field,
                        std::string(R"(,"z":"0.0")").size());
    ASSERT_EQ(p.parse(no_cumulative, out), execution_parse_result::valid);
    EXPECT_FALSE(out.has_cumulative_qty);
}

TEST(BinanceUserDataParser, Canceled)
{
    BinanceUserDataParser p;
    parsed_exec out;
    ASSERT_EQ(p.parse(report("CANCELED", "CANCELED"), out),
              execution_parse_result::valid);
    EXPECT_EQ(out.k, parsed_exec::kind::canceled);

    auto missing_cumulative = report("CANCELED", "CANCELED");
    const auto cumulative_field = missing_cumulative.find(R"(,"z":"0.0")");
    ASSERT_NE(cumulative_field, std::string::npos);
    missing_cumulative.erase(cumulative_field,
                             std::string(R"(,"z":"0.0")").size());
    EXPECT_EQ(p.parse(missing_cumulative, out), execution_parse_result::malformed);
}

TEST(BinanceUserDataParser, Rejected)
{
    BinanceUserDataParser p;
    parsed_exec out;
    auto j = report("REJECTED", "REJECTED", "0", "0", "0",
                    "tt-2", "BUY", "0", "0", "USDT",
                    "INSUFFICIENT_BALANCE");
    ASSERT_EQ(p.parse(j, out), execution_parse_result::valid);
    EXPECT_EQ(out.k, parsed_exec::kind::rejected);
    EXPECT_EQ(out.error, "INSUFFICIENT_BALANCE");
}

TEST(BinanceUserDataParser, Expired)
{
    BinanceUserDataParser p;
    parsed_exec out;
    ASSERT_EQ(p.parse(report("EXPIRED", "EXPIRED"), out),
              execution_parse_result::valid);
    EXPECT_EQ(out.k, parsed_exec::kind::expired);
}

TEST(BinanceUserDataParser, StpAndReplacementLifecyclesRemainValid)
{
    BinanceUserDataParser p;
    parsed_exec out;

    ASSERT_EQ(p.parse(report("TRADE_PREVENTION", "EXPIRED_IN_MATCH"), out),
              execution_parse_result::valid);
    EXPECT_EQ(out.k, parsed_exec::kind::expired);

    ASSERT_EQ(p.parse(report("EXPIRED", "EXPIRED_IN_MATCH"), out),
              execution_parse_result::valid);
    EXPECT_EQ(out.k, parsed_exec::kind::expired);

    ASSERT_EQ(p.parse(report("REPLACED", "PENDING_NEW"), out),
              execution_parse_result::valid);
    EXPECT_EQ(out.k, parsed_exec::kind::ack);
}

TEST(BinanceUserDataParser, TimestampFromEventMillis)
{
    BinanceUserDataParser p;
    parsed_exec out;
    ASSERT_EQ(p.parse(report("NEW", "NEW"), out),
              execution_parse_result::valid);

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  out.ts.time_since_epoch()).count();
    EXPECT_EQ(ms, 1700000000000LL);
}

TEST(BinanceUserDataParser, NumericStringExchangeIdIsAccepted)
{
    BinanceUserDataParser p;
    parsed_exec out;
    std::string j = R"({"e":"executionReport","E":1,"s":"X","c":"c1","S":"BUY",)"
                    R"("x":"NEW","X":"NEW","i":"77",)"
                    R"("l":"0","L":"0","z":"0","n":"0","N":"USDT"})";
    ASSERT_EQ(p.parse(j, out), execution_parse_result::valid);
    EXPECT_EQ(out.exchange_order_id, "77");
}
