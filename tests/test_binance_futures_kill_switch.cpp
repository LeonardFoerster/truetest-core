#include <gtest/gtest.h>

#ifdef HAS_BINANCE

#include "providers/binance/binance_futures_kill_switch.h"
#include "providers/binance/binance_kill_switch.h"

#include <chrono>
#include <sstream>
#include <vector>

namespace {

// RAII helper to silence stderr - the kill switch logs deliberately
// noisy diagnostics on the failure paths we exercise here.
struct SilenceStderr {
    std::ostringstream sink;
    std::streambuf* orig;
    SilenceStderr() : orig(std::cerr.rdbuf(sink.rdbuf())) {}
    ~SilenceStderr() { std::cerr.rdbuf(orig); }
};

}

TEST(BinanceFuturesKillSwitch, NullRestReturnsFalseAndDoesNotCrash)
{
    SilenceStderr quiet;
    BinanceFuturesKillSwitch ks(nullptr, "BTCUSDT", nullptr);
    EXPECT_FALSE(ks.cancel_all_and_flatten(std::chrono::milliseconds(1000)));
}

TEST(BinanceFuturesKillSwitch, ImplementsIKillSwitchInterface)
{
    // Compile-time check: BinanceFuturesKillSwitch must be substitutable
    // for an IKillSwitch (the engine queries via the base interface).
    BinanceFuturesKillSwitch ks(nullptr, "BTCUSDT", nullptr);
    IKillSwitch& base = ks;
    SilenceStderr quiet;
    EXPECT_FALSE(base.cancel_all_and_flatten(std::chrono::milliseconds(1)));
}

TEST(BinanceFuturesKillSwitch, CancelFailureStillAttemptsReduceOnlyFlatten)
{
    SilenceStderr quiet;
    std::vector<std::pair<std::string, std::string>> calls;
    auto del = [&](const std::string& ep, const std::string& params,
                   std::chrono::milliseconds) {
        calls.emplace_back(ep, params);
        return BinanceRestClient::response{500, "cancel failed"};
    };
    auto get = [&](const std::string& ep, const std::string& params,
                   std::chrono::milliseconds) {
        calls.emplace_back(ep, params);
        return BinanceRestClient::response{
            200, R"([{"symbol":"BTCUSDT","positionAmt":"2.5"}])"};
    };
    auto post = [&](const std::string& ep, const std::string& params,
                    std::chrono::milliseconds) {
        calls.emplace_back(ep, params);
        return BinanceRestClient::response{200, R"({"orderId":1})"};
    };
    BinanceFuturesKillSwitch ks(
        BinanceFuturesKillSwitch::injected_requests,
        del, get, post, "BTCUSDT");

    EXPECT_FALSE(ks.cancel_all_and_flatten(std::chrono::seconds(1)));
    ASSERT_EQ(calls.size(), 5u);
    EXPECT_EQ(calls[0].first, "/fapi/v1/allOpenOrders");
    EXPECT_EQ(calls[1].first, "/fapi/v1/algoOpenOrders");
    EXPECT_NE(calls[3].second.find("side=SELL"), std::string::npos);
    EXPECT_NE(calls[3].second.find("reduceOnly=true"), std::string::npos);
    EXPECT_NE(calls[3].second.find("quantity=2.50000000"), std::string::npos);
}

TEST(BinanceFuturesKillSwitch, WrongSymbolPositionRefusesFlatten)
{
    SilenceStderr quiet;
    int posts = 0;
    auto ok_del = [](const std::string& endpoint, const std::string&,
                     std::chrono::milliseconds) {
        return endpoint == "/fapi/v1/algoOpenOrders"
            ? BinanceRestClient::response{200, R"({"code":200,"msg":"The operation of cancel all open order is done."})"}
            : BinanceRestClient::response{200, R"({"code":200,"msg":"The operation of cancel all open order is done."})"};
    };
    auto wrong_get = [](const std::string&, const std::string&,
                        std::chrono::milliseconds) {
        return BinanceRestClient::response{
            200, R"([{"symbol":"ETHUSDT","positionAmt":"0"}])"};
    };
    auto post = [&](const std::string&, const std::string&,
                    std::chrono::milliseconds) {
        ++posts;
        return BinanceRestClient::response{200, R"({"orderId":1})"};
    };
    BinanceFuturesKillSwitch ks(
        BinanceFuturesKillSwitch::injected_requests,
        ok_del, wrong_get, post, "BTCUSDT");
    EXPECT_FALSE(ks.cancel_all_and_flatten(std::chrono::seconds(1)));
    EXPECT_EQ(posts, 0);
}

TEST(BinanceFuturesKillSwitch, FlattenRequiresAuthoritativeFlatReadback)
{
    auto cancel = [](const std::string& endpoint, const std::string&,
                     std::chrono::milliseconds) {
        return endpoint == "/fapi/v1/algoOpenOrders"
            ? BinanceRestClient::response{200, R"({"code":200,"msg":"The operation of cancel all open order is done."})"}
            : BinanceRestClient::response{200, R"({"code":200,"msg":"The operation of cancel all open order is done."})"};
    };
    int reads = 0;
    auto eventually_flat = [&](const std::string&, const std::string&,
                               std::chrono::milliseconds) {
        ++reads;
        return BinanceRestClient::response{
            200, reads == 1
                ? R"([{"symbol":"BTCUSDT","positionAmt":"1"}])"
                : R"([{"symbol":"BTCUSDT","positionAmt":"0"}])"};
    };
    auto post = [](const std::string&, const std::string&,
                   std::chrono::milliseconds) {
        return BinanceRestClient::response{200, R"({"orderId":7})"};
    };
    BinanceFuturesKillSwitch success(
        BinanceFuturesKillSwitch::injected_requests,
        cancel, eventually_flat, post, "BTCUSDT");
    EXPECT_TRUE(success.cancel_all_and_flatten(std::chrono::seconds(1)));
    EXPECT_EQ(reads, 2);

    auto never_flat = [](const std::string&, const std::string&,
                         std::chrono::milliseconds) {
        return BinanceRestClient::response{
            200, R"([{"symbol":"BTCUSDT","positionAmt":"1"}])"};
    };
    BinanceFuturesKillSwitch failure(
        BinanceFuturesKillSwitch::injected_requests,
        cancel, never_flat, post, "BTCUSDT");
    EXPECT_FALSE(failure.cancel_all_and_flatten(std::chrono::seconds(1)));
}

TEST(BinanceKillSwitch, CancelFailureAndLockedInventoryStillSellFreeButFail)
{
    SilenceStderr quiet;
    int posts = 0;
    auto del = [](const std::string&, const std::string&,
                  std::chrono::milliseconds) {
        return BinanceRestClient::response{500, "cancel failed"};
    };
    auto get = [](const std::string&, const std::string&,
                  std::chrono::milliseconds) {
        return BinanceRestClient::response{
            200, R"({"balances":[{"asset":"BTC","free":"1.25","locked":"0.5"}]})"};
    };
    auto post = [&](const std::string&, const std::string& params,
                    std::chrono::milliseconds) {
        ++posts;
        EXPECT_NE(params.find("side=SELL"), std::string::npos);
        EXPECT_NE(params.find("quantity=1.25000000"), std::string::npos);
        return BinanceRestClient::response{200, R"({"orderId":1})"};
    };
    BinanceKillSwitch ks(
        BinanceKillSwitch::injected_requests,
        del, get, post, "BTCUSDT", "BTC");
    EXPECT_FALSE(ks.cancel_all_and_flatten(std::chrono::seconds(1)));
    EXPECT_EQ(posts, 1);
}

TEST(BinanceFuturesKillSwitch, ThrowingCancelStillAttemptsFlatten)
{
    SilenceStderr quiet;
    int gets = 0;
    int posts = 0;
    auto del = [](const std::string&, const std::string&,
                  std::chrono::milliseconds) -> BinanceRestClient::response {
        throw std::runtime_error("cancel transport failed");
    };
    auto get = [&](const std::string&, const std::string&,
                   std::chrono::milliseconds) {
        ++gets;
        return BinanceRestClient::response{
            200, R"([{"symbol":"BTCUSDT","positionAmt":"-2"}])"};
    };
    auto post = [&](const std::string&, const std::string& params,
                    std::chrono::milliseconds) {
        ++posts;
        EXPECT_NE(params.find("side=BUY"), std::string::npos);
        return BinanceRestClient::response{200, R"({"orderId":1})"};
    };
    BinanceFuturesKillSwitch ks(
        BinanceFuturesKillSwitch::injected_requests,
        del, get, post, "BTCUSDT");
    EXPECT_FALSE(ks.cancel_all_and_flatten(std::chrono::seconds(1)));
    EXPECT_EQ(gets, 2);
    EXPECT_EQ(posts, 1);
}

TEST(BinanceKillSwitch, ThrowingCancelStillAttemptsFreeInventorySale)
{
    SilenceStderr quiet;
    int gets = 0;
    int posts = 0;
    auto del = [](const std::string&, const std::string&,
                  std::chrono::milliseconds) -> BinanceRestClient::response {
        throw std::runtime_error("cancel transport failed");
    };
    auto get = [&](const std::string&, const std::string&,
                   std::chrono::milliseconds) {
        ++gets;
        return BinanceRestClient::response{
            200, R"({"balances":[{"asset":"BTC","free":"1","locked":"0"}]})"};
    };
    auto post = [&](const std::string&, const std::string&,
                    std::chrono::milliseconds) {
        ++posts;
        return BinanceRestClient::response{200, R"({"orderId":1})"};
    };
    BinanceKillSwitch ks(BinanceKillSwitch::injected_requests,
                         del, get, post, "BTCUSDT", "BTC");
    EXPECT_FALSE(ks.cancel_all_and_flatten(std::chrono::seconds(1)));
    EXPECT_EQ(gets, 2);
    EXPECT_EQ(posts, 1);
}

TEST(BinanceKillSwitch, FlattenRequiresAuthoritativeZeroBalanceReadback)
{
    auto cancel = [](const std::string&, const std::string&,
                     std::chrono::milliseconds) {
        return BinanceRestClient::response{
            200, R"([{"symbol":"BTCUSDT","orderId":5,"status":"CANCELED"}])"};
    };
    int reads = 0;
    auto eventually_flat = [&](const std::string&, const std::string&,
                               std::chrono::milliseconds) {
        ++reads;
        BinanceRestClient::response response{
            200, reads == 1
                ? R"({"balances":[{"asset":"BTC","free":"1","locked":"0"}]})"
                : R"({"balances":[{"asset":"BTC","free":"0","locked":"0"}]})"};
        double free = 0.0;
        double locked = 0.0;
        EXPECT_TRUE(BinanceReconciler::extract_balance(
            response.body, "BTC", free, locked));
        EXPECT_DOUBLE_EQ(free, reads == 1 ? 1.0 : 0.0);
        return response;
    };
    auto post = [](const std::string&, const std::string&,
                   std::chrono::milliseconds) {
        return BinanceRestClient::response{200, R"({"orderId":7})"};
    };
    BinanceKillSwitch success(BinanceKillSwitch::injected_requests,
        cancel, eventually_flat, post, "BTCUSDT", "BTC");
    EXPECT_TRUE(success.cancel_all_and_flatten(std::chrono::seconds(1)));
    EXPECT_EQ(reads, 2);

    auto never_flat = [](const std::string&, const std::string&,
                         std::chrono::milliseconds) {
        return BinanceRestClient::response{
            200, R"({"balances":[{"asset":"BTC","free":"1","locked":"0"}]})"};
    };
    BinanceKillSwitch failure(BinanceKillSwitch::injected_requests,
        cancel, never_flat, post, "BTCUSDT", "BTC");
    EXPECT_FALSE(failure.cancel_all_and_flatten(std::chrono::seconds(1)));
}

TEST(BinanceKillSwitch, CancelNoopRequiresExactAuthoritativeCode)
{
    SilenceStderr quiet;
    auto del = [](const std::string&, const std::string&,
                  std::chrono::milliseconds) {
        return BinanceRestClient::response{
            500, R"({"code":-1000,"msg":"upstream -2011 text"})"};
    };
    auto get = [](const std::string&, const std::string&,
                  std::chrono::milliseconds) {
        return BinanceRestClient::response{
            200, R"({"balances":[{"asset":"BTC","free":"0","locked":"0"}]})"};
    };
    auto post = [](const std::string&, const std::string&,
                   std::chrono::milliseconds) {
        return BinanceRestClient::response{200, "{}"};
    };
    BinanceKillSwitch ks(BinanceKillSwitch::injected_requests,
                         del, get, post, "BTCUSDT", "BTC");
    EXPECT_FALSE(ks.cancel_all_and_flatten(std::chrono::seconds(1)));
}

TEST(BinanceFuturesKillSwitch, RequiresAuthoritativeCancelAndFlattenAcks)
{
    SilenceStderr quiet;
    auto flat = [](const std::string&, const std::string&,
                   std::chrono::milliseconds) {
        return BinanceRestClient::response{
            200, R"([{"symbol":"BTCUSDT","positionAmt":"0"}])"};
    };
    auto malformed_cancel = [](const std::string&, const std::string&,
                               std::chrono::milliseconds) {
        return BinanceRestClient::response{200, "{}"};
    };
    auto unused_post = [](const std::string&, const std::string&,
                          std::chrono::milliseconds) {
        return BinanceRestClient::response{200, R"({"orderId":1})"};
    };
    BinanceFuturesKillSwitch bad_cancel(
        BinanceFuturesKillSwitch::injected_requests,
        malformed_cancel, flat, unused_post, "BTCUSDT");
    EXPECT_FALSE(bad_cancel.cancel_all_and_flatten(std::chrono::seconds(1)));

    auto cancel_ok = [](const std::string& endpoint, const std::string&,
                        std::chrono::milliseconds) {
        return endpoint == "/fapi/v1/algoOpenOrders"
            ? BinanceRestClient::response{200, R"({"code":200,"msg":"The operation of cancel all open order is done."})"}
            : BinanceRestClient::response{200, R"({"code":200,"msg":"The operation of cancel all open order is done."})"};
    };
    auto exposed = [](const std::string&, const std::string&,
                      std::chrono::milliseconds) {
        return BinanceRestClient::response{
            200, R"([{"symbol":"BTCUSDT","positionAmt":"1"}])"};
    };
    auto malformed_flatten = [](const std::string&, const std::string&,
                                std::chrono::milliseconds) {
        return BinanceRestClient::response{
            200, R"({"nested":{"orderId":7,"clientOrderId":"tt-kill-1"}})"};
    };
    BinanceFuturesKillSwitch bad_flatten(
        BinanceFuturesKillSwitch::injected_requests,
        cancel_ok, exposed, malformed_flatten, "BTCUSDT");
    EXPECT_FALSE(bad_flatten.cancel_all_and_flatten(std::chrono::seconds(1)));
}

TEST(BinanceKillSwitch, RequiresAuthoritativeCancelAndFlattenAcks)
{
    SilenceStderr quiet;
    auto flat = [](const std::string&, const std::string&,
                   std::chrono::milliseconds) {
        return BinanceRestClient::response{
            200, R"({"balances":[{"asset":"BTC","free":"0","locked":"0"}]})"};
    };
    auto malformed_cancel = [](const std::string&, const std::string&,
                               std::chrono::milliseconds) {
        return BinanceRestClient::response{200, "{}"};
    };
    auto unused_post = [](const std::string&, const std::string&,
                          std::chrono::milliseconds) {
        return BinanceRestClient::response{200, R"({"orderId":1})"};
    };
    BinanceKillSwitch bad_cancel(BinanceKillSwitch::injected_requests,
        malformed_cancel, flat, unused_post, "BTCUSDT", "BTC");
    EXPECT_FALSE(bad_cancel.cancel_all_and_flatten(std::chrono::seconds(1)));

    auto cancel_ok = [](const std::string&, const std::string&,
                        std::chrono::milliseconds) {
        return BinanceRestClient::response{200, "[]"};
    };
    auto exposed = [](const std::string&, const std::string&,
                      std::chrono::milliseconds) {
        return BinanceRestClient::response{
            200, R"({"balances":[{"asset":"BTC","free":"1","locked":"0"}]})"};
    };
    auto malformed_flatten = [](const std::string&, const std::string&,
                                std::chrono::milliseconds) {
        return BinanceRestClient::response{
            200, R"({"nested":{"orderId":7,"clientOrderId":"tt-kill-1"}})"};
    };
    BinanceKillSwitch bad_flatten(BinanceKillSwitch::injected_requests,
        cancel_ok, exposed, malformed_flatten, "BTCUSDT", "BTC");
    EXPECT_FALSE(bad_flatten.cancel_all_and_flatten(std::chrono::seconds(1)));
}

TEST(BinanceKillSwitch, AcceptsAuthoritativeOcoCancelAllRow)
{
    auto cancel_oco = [](const std::string&, const std::string&,
                         std::chrono::milliseconds) {
        return BinanceRestClient::response{200, R"([{"orderListId":1929,"contingencyType":"OCO","listStatusType":"ALL_DONE","listOrderStatus":"ALL_DONE","listClientOrderId":"tt-oco","transactionTime":1565245913483,"symbol":"BTCUSDT","orders":[{"symbol":"BTCUSDT","orderId":2,"clientOrderId":"a"},{"symbol":"BTCUSDT","orderId":3,"clientOrderId":"b"}],"orderReports":[{"symbol":"BTCUSDT","orderId":2,"status":"CANCELED"},{"symbol":"BTCUSDT","orderId":3,"status":"CANCELED"}]}])"};
    };
    auto flat = [](const std::string&, const std::string&,
                   std::chrono::milliseconds) {
        return BinanceRestClient::response{
            200, R"({"balances":[{"asset":"BTC","free":"0","locked":"0"}]})"};
    };
    auto unused_post = [](const std::string&, const std::string&,
                          std::chrono::milliseconds) {
        return BinanceRestClient::response{200, R"({"orderId":1})"};
    };
    BinanceKillSwitch ks(BinanceKillSwitch::injected_requests,
        cancel_oco, flat, unused_post, "BTCUSDT", "BTC");
    EXPECT_TRUE(ks.cancel_all_and_flatten(std::chrono::seconds(1)));
}

TEST(BinanceKillSwitch, RefusesUnprovenOcoCancelAllRow)
{
    SilenceStderr quiet;
    auto malformed_oco = [](const std::string&, const std::string&,
                            std::chrono::milliseconds) {
        return BinanceRestClient::response{200, R"([{"orderListId":1929,"listStatusType":"EXEC_STARTED","listOrderStatus":"EXECUTING","symbol":"BTCUSDT","orderReports":[{"symbol":"BTCUSDT","orderId":2,"status":"CANCELED"}]}])"};
    };
    auto flat = [](const std::string&, const std::string&,
                   std::chrono::milliseconds) {
        return BinanceRestClient::response{
            200, R"({"balances":[{"asset":"BTC","free":"0","locked":"0"}]})"};
    };
    auto unused_post = [](const std::string&, const std::string&,
                          std::chrono::milliseconds) {
        return BinanceRestClient::response{200, R"({"orderId":1})"};
    };
    BinanceKillSwitch ks(BinanceKillSwitch::injected_requests,
        malformed_oco, flat, unused_post, "BTCUSDT", "BTC");
    EXPECT_FALSE(ks.cancel_all_and_flatten(std::chrono::seconds(1)));
}

#endif // HAS_BINANCE
