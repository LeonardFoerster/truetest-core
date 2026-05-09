// Opt-in integration test against the Binance USDT-M futures testnet.
//
// Skipped unless TRUETEST_FUTURES_TESTNET_KEY and
// TRUETEST_FUTURES_TESTNET_SECRET are set. Different keys from spot —
// futures testnet has a separate signup at testnet.binancefuture.com.
//
// What it covers, in order:
//   - clock resync against /fapi/v1/time (the path PR 1 made injectable)
//   - /fapi/v1/exchangeInfo probe (the symbol-existence gate)
//   - /fapi/v1/positionSide/dual readback (the one-way-mode gate)
//   - mark price via /fapi/v1/ticker/price
//   - place a LIMIT BUY at half mark so it can't fill
//   - cancel by clientOrderId
//
// User-data ORDER_TRADE_UPDATE is async and out of scope here — covered
// by the unit-level parser tests. The point of this file is to assert
// our wire format matches what the real venue accepts.

#include <gtest/gtest.h>

#ifdef HAS_BINANCE

#include "providers/binance/binance_endpoints.h"
#include "providers/binance/binance_parser.h"
#include "providers/binance/binance_rest_client.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

std::string env_or_empty(const char* name)
{
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string{};
}

} // namespace

TEST(BinanceFuturesTestnetLive, PlaceAndCancelLimitOrder)
{
    const auto key = env_or_empty("TRUETEST_FUTURES_TESTNET_KEY");
    const auto sec = env_or_empty("TRUETEST_FUTURES_TESTNET_SECRET");
    if (key.empty() || sec.empty())
    {
        std::fprintf(stderr,
            "  (BinanceFuturesTestnetLive: "
            "TRUETEST_FUTURES_TESTNET_KEY/SECRET unset, "
            "skipping live testnet round-trip)\n");
        return;
    }

    const auto ep = binance::usdm_testnet();
    // Time path injection from PR 1 — futures has /fapi/v1/time, not
    // /api/v3/time, and the rest client must use it for clock skew.
    BinanceRestClient cli(key, sec, ep.rest_host, ep.rest_port,
                          "/fapi/v1/time");

    ASSERT_TRUE(cli.resync_clock_now())
        << "clock resync failed — futures testnet unreachable or DNS broken";

    auto info = cli.get_unsigned("/fapi/v1/exchangeInfo", "symbol=BTCUSDT");
    ASSERT_GE(info.status, 200);
    ASSERT_LT(info.status, 300) << "exchangeInfo HTTP " << info.status
                                << ": " << info.body;

    // Position-mode probe is signed (USER_DATA endpoint). The provider
    // refuses to go live if dualSidePosition is true; this test asserts
    // we can read the field cleanly on a live account.
    auto pmode = cli.get("/fapi/v1/positionSide/dual", "");
    ASSERT_GE(pmode.status, 200);
    ASSERT_LT(pmode.status, 300) << "positionSide/dual HTTP " << pmode.status
                                 << ": " << pmode.body;
    ASSERT_NE(pmode.body.find("dualSidePosition"), std::string::npos)
        << "positionSide/dual response missing field: " << pmode.body;

    auto px = cli.get_unsigned("/fapi/v1/ticker/price", "symbol=BTCUSDT");
    ASSERT_GE(px.status, 200);
    ASSERT_LT(px.status, 300) << "ticker/price HTTP " << px.status
                              << ": " << px.body;

    double mark = 0.0;
    auto px_sv = binance::extract_sv_string(px.body, "price");
    ASSERT_TRUE(binance::parse_double_sv(px_sv, mark)) << px.body;
    ASSERT_GT(mark, 0.0);

    char price_buf[32];
    std::snprintf(price_buf, sizeof(price_buf), "%.1f", mark * 0.5);

    const std::string client_id =
        "tt-fut-it-" + std::to_string(static_cast<long long>(mark));

    // One-way mode: positionSide is omitted (Binance defaults to BOTH).
    // 0.001 BTC clears the futures BTCUSDT LOT_SIZE filter (0.001 step).
    const std::string place_params =
        std::string("symbol=BTCUSDT&side=BUY&type=LIMIT&timeInForce=GTC")
        + "&quantity=0.001&price=" + price_buf
        + "&newClientOrderId=" + client_id;

    auto place = cli.post("/fapi/v1/order", place_params);
    ASSERT_GE(place.status, 200);
    ASSERT_LT(place.status, 300) << "place HTTP " << place.status
                                 << ": " << place.body;

    auto cancel = cli.del(
        "/fapi/v1/order",
        "symbol=BTCUSDT&origClientOrderId=" + client_id);
    EXPECT_GE(cancel.status, 200);
    EXPECT_LT(cancel.status, 300) << "cancel HTTP " << cancel.status
                                  << ": " << cancel.body;
}

#endif // HAS_BINANCE
