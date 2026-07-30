#include <gtest/gtest.h>

#ifdef HAS_GATE

#include "providers/gate/gate_futures_reconciler.h"

#include <string>
#include <unordered_map>

namespace {

GateRestClient::response ok_body(std::string body)
{
    GateRestClient::response r;
    r.status = 200;
    r.body = std::move(body);
    r.business_ok = true;
    return r;
}

GateRestClient::response http_fail(int status, std::string body = "err")
{
    GateRestClient::response r;
    r.status = status;
    r.body = std::move(body);
    r.business_ok = false;
    return r;
}

// Gate USDT-M accounts object (available is free cash).
std::string accounts_json(const char* available,
                          const char* dual = "false")
{
    return std::string(R"({"user":1,"currency":"USDT","total":")")
         + available + R"(","available":")" + available
         + R"(","unrealised_pnl":"0","position_margin":"0",)"
           R"("order_margin":"0","in_dual_mode":)"
         + dual + R"(,"position_mode":"single"})";
}

// Single-position object (GET .../positions/BTC_USDT).
std::string pos_object(const char* size, const char* contract = "BTC_USDT")
{
    return std::string(R"({"user":1,"contract":")") + contract
         + R"(","size":)" + size
         + R"(,"leverage":"0","margin":"10","entry_price":"50000",)"
           R"("liq_price":"40000","mark_price":"51000","mode":"single"})";
}

// List endpoint body.
std::string pos_list(const char* size, const char* contract = "BTC_USDT")
{
    return std::string("[") + pos_object(size, contract) + "]";
}

std::string pos_empty_list()
{
    return "[]";
}

std::string pos_not_found()
{
    return R"({"label":"POSITION_NOT_FOUND","message":"Position not found"})";
}

} // namespace

// --- extract_available ---

TEST(GateFuturesReconciler, AvailableFromAccountsObject)
{
    const std::string body = accounts_json("75.25");
    double out = 0.0;
    ASSERT_TRUE(GateFuturesReconciler::extract_available(body, out));
    EXPECT_DOUBLE_EQ(out, 75.25);
}

TEST(GateFuturesReconciler, AvailableNumericField)
{
    std::string body = R"({"currency":"USDT","available":1000.5})";
    double out = 0.0;
    ASSERT_TRUE(GateFuturesReconciler::extract_available(body, out));
    EXPECT_DOUBLE_EQ(out, 1000.5);
}

TEST(GateFuturesReconciler, AvailableMissingReturnsFalse)
{
    std::string body = R"({"currency":"USDT","total":"100"})";
    double out = 42.0;
    EXPECT_FALSE(GateFuturesReconciler::extract_available(body, out));
}

// --- extract_position_size ---

TEST(GateFuturesReconciler, PositionSizePositiveLong)
{
    double out = 0.0;
    ASSERT_TRUE(GateFuturesReconciler::extract_position_size(
        pos_object("0.50"), out, "BTC_USDT"));
    EXPECT_DOUBLE_EQ(out, 0.50);
}

TEST(GateFuturesReconciler, PositionSizeNegativeShort)
{
    // Gate size is signed: short < 0.
    double out = 0.0;
    ASSERT_TRUE(GateFuturesReconciler::extract_position_size(
        pos_object("-1.25"), out, "BTC_USDT"));
    EXPECT_DOUBLE_EQ(out, -1.25);
}

TEST(GateFuturesReconciler, PositionSizeEmptyListIsFlatZero)
{
    double out = 99.0;
    ASSERT_TRUE(GateFuturesReconciler::extract_position_size(
        pos_empty_list(), out, "BTC_USDT"));
    EXPECT_DOUBLE_EQ(out, 0.0);
}

TEST(GateFuturesReconciler, PositionSizeNotFoundLabelIsFlatZero)
{
    double out = 99.0;
    ASSERT_TRUE(GateFuturesReconciler::extract_position_size(
        pos_not_found(), out, "BTC_USDT"));
    EXPECT_DOUBLE_EQ(out, 0.0);
}

TEST(GateFuturesReconciler, PositionSizeOtherSymbolsOnlyRefuses)
{
    const std::string body = pos_list("1.5", "ETH_USDT");
    double out = 99.0;
    EXPECT_FALSE(GateFuturesReconciler::extract_position_size(
        body, out, "BTC_USDT"));
}

TEST(GateFuturesReconciler, PositionSizeWrongSymbolSingleObjectRefuses)
{
    double out = 99.0;
    EXPECT_FALSE(GateFuturesReconciler::extract_position_size(
        pos_object("2.0", "ETH_USDT"), out, "BTC_USDT"));
}

TEST(GateFuturesReconciler, PositionSizeZeroIsParseable)
{
    double out = 99.0;
    ASSERT_TRUE(GateFuturesReconciler::extract_position_size(
        pos_object("0"), out, "BTC_USDT"));
    EXPECT_DOUBLE_EQ(out, 0.0);
}

TEST(GateFuturesReconciler, PositionSizeFromListMatchesSymbol)
{
    double out = 0.0;
    ASSERT_TRUE(GateFuturesReconciler::extract_position_size(
        pos_list("-3"), out, "BTC_USDT"));
    EXPECT_DOUBLE_EQ(out, -3.0);
}

TEST(GateFuturesReconciler, PositionSizeMissingFieldRefuses)
{
    std::string body =
        R"({"contract":"BTC_USDT","entry_price":"30000","mode":"single"})";
    double out = 42.0;
    EXPECT_FALSE(GateFuturesReconciler::extract_position_size(
        body, out, "BTC_USDT"));
}

// --- reconcile via injected get_fn ---

TEST(GateFuturesReconciler, NullGetReturnsError)
{
    GateFuturesReconciler r(GateFuturesReconciler::get_fn{}, "BTC_USDT");
    portfolio p(100.0);
    auto note = r.reconcile(p, /*tolerance_bps=*/100.0);
    EXPECT_NE(note.find("no REST client"), std::string::npos);
}

TEST(GateFuturesReconciler, MatchPasses)
{
    auto get = [](const std::string& path, const std::string& /*q*/) {
        if (path.find("/positions/") != std::string::npos)
            return ok_body(pos_object("0.1"));
        if (path.find("/accounts") != std::string::npos)
            return ok_body(accounts_json("1000"));
        return http_fail(404);
    };

    GateFuturesReconciler r(get, "BTC_USDT");
    portfolio p;
    std::unordered_map<std::string, position> pos;
    pos["BTC_USDT"] = position{0.1, 0.0};
    p.restore_state(/*cash=*/1000.0, /*trades=*/0, std::move(pos));

    EXPECT_TRUE(r.reconcile(p, /*tolerance_bps=*/10.0).empty());
}

TEST(GateFuturesReconciler, MatchPassesWhenPositionNotFoundAndLocalFlat)
{
    auto get = [](const std::string& path, const std::string& /*q*/) {
        if (path.find("/positions/") != std::string::npos)
            return http_fail(404, pos_not_found());
        if (path.find("/accounts") != std::string::npos)
            return ok_body(accounts_json("500"));
        return http_fail(404);
    };

    GateFuturesReconciler r(get, "BTC_USDT");
    portfolio p(500.0); // flat, cash matches
    EXPECT_TRUE(r.reconcile(p, /*tolerance_bps=*/10.0).empty());
}

TEST(GateFuturesReconciler, CashMismatchFails)
{
    auto get = [](const std::string& path, const std::string& /*q*/) {
        if (path.find("/positions/") != std::string::npos)
            return http_fail(404, pos_not_found());
        if (path.find("/accounts") != std::string::npos)
            return ok_body(accounts_json("50"));
        return http_fail(404);
    };

    GateFuturesReconciler r(get, "BTC_USDT");
    portfolio p(1000.0); // local cash 1000 vs venue 50
    auto note = r.reconcile(p, /*tolerance_bps=*/10.0);
    EXPECT_NE(note.find("cash drift"), std::string::npos);
}

TEST(GateFuturesReconciler, PositionMismatchFails)
{
    auto get = [](const std::string& path, const std::string& /*q*/) {
        if (path.find("/positions/") != std::string::npos)
            return ok_body(pos_object("2.0"));
        if (path.find("/accounts") != std::string::npos)
            return ok_body(accounts_json("1000"));
        return http_fail(404);
    };

    GateFuturesReconciler r(get, "BTC_USDT");
    portfolio p;
    std::unordered_map<std::string, position> pos;
    pos["BTC_USDT"] = position{0.1, 0.0}; // local 0.1 vs venue 2.0
    p.restore_state(/*cash=*/1000.0, /*trades=*/0, std::move(pos));

    auto note = r.reconcile(p, /*tolerance_bps=*/10.0);
    EXPECT_NE(note.find("position drift"), std::string::npos);
}

TEST(GateFuturesReconciler, HttpFailureFailsClosed)
{
    auto get = [](const std::string& path, const std::string& /*q*/) {
        if (path.find("/positions/") != std::string::npos)
            return http_fail(503, R"({"label":"SERVER_ERROR","message":"unavailable"})");
        return ok_body(accounts_json("1000"));
    };

    GateFuturesReconciler r(get, "BTC_USDT");
    portfolio p(1000.0);
    auto note = r.reconcile(p, 100.0);
    EXPECT_NE(note.find("HTTP 503"), std::string::npos);
}

TEST(GateFuturesReconciler, AccountsHttpFailureFailsClosed)
{
    auto get = [](const std::string& path, const std::string& /*q*/) {
        if (path.find("/positions/") != std::string::npos)
            return ok_body(pos_object("0"));
        if (path.find("/accounts") != std::string::npos)
            return http_fail(401, R"({"label":"INVALID_KEY","message":"bad key"})");
        return http_fail(404);
    };

    GateFuturesReconciler r(get, "BTC_USDT");
    portfolio p(1000.0);
    auto note = r.reconcile(p, 100.0);
    EXPECT_NE(note.find("HTTP 401"), std::string::npos);
    EXPECT_NE(note.find("accounts"), std::string::npos);
}

TEST(GateFuturesReconciler, IsTestnetReflectsEndpoints)
{
    GateFuturesReconciler m(GateFuturesReconciler::get_fn{}, "BTC_USDT",
                            gate::usdt_mainnet());
    GateFuturesReconciler t(GateFuturesReconciler::get_fn{}, "BTC_USDT",
                            gate::usdt_testnet());
    EXPECT_FALSE(m.is_testnet());
    EXPECT_TRUE(t.is_testnet());
}

TEST(GateFuturesReconciler, PathsUseSettlePrefix)
{
    std::string seen_pos;
    std::string seen_acct;
    auto get = [&](const std::string& path, const std::string& /*q*/) {
        if (path.find("/positions/") != std::string::npos)
        {
            seen_pos = path;
            return ok_body(pos_object("0"));
        }
        if (path.find("/accounts") != std::string::npos)
        {
            seen_acct = path;
            return ok_body(accounts_json("100"));
        }
        return http_fail(404);
    };

    GateFuturesReconciler r(get, "BTC_USDT", gate::usdt_mainnet());
    portfolio p(100.0);
    EXPECT_TRUE(r.reconcile(p, 10.0).empty());
    EXPECT_EQ(seen_pos, "/api/v4/futures/usdt/positions/BTC_USDT");
    EXPECT_EQ(seen_acct, "/api/v4/futures/usdt/accounts");
}

#endif // HAS_GATE
