// Opt-in integration tests against the Binance USDT-M futures testnet.
//
// Skipped unless TRUETEST_FUTURES_TESTNET_KEY and
// TRUETEST_FUTURES_TESTNET_SECRET are set. Different keys from spot —
// futures testnet has a separate signup at testnet.binancefuture.com.
//
// What it covers, across four TEST blocks:
//   - Place + cancel — symbol probe, position-mode probe, place a
//     LIMIT BUY at half mark so it can't fill, cancel by clientOrderId.
//   - listenKey lifecycle — POST + PUT + DELETE round-trip on
//     /fapi/v1/listenKey.
//   - Bracket adapter round-trip — place SL+TP far from market,
//     list_open recovers the pair, cancel + list_open shows it gone.
//   - Reconciler round-trip — build a reconciler against the live
//     account, call reconcile() with an empty portfolio at very high
//     tolerance, assert it doesn't throw or hang.
//
// User-data WebSocket events (ORDER_TRADE_UPDATE, ACCOUNT_UPDATE) are
// async and out of scope here — covered by unit-level parser tests.
// The point of this file is to assert our wire format matches what
// the real venue accepts.

#include <gtest/gtest.h>

#ifdef HAS_BINANCE

#include "providers/binance/binance_endpoints.h"
#include "providers/binance/binance_futures_bracket_adapter.h"
#include "providers/binance/binance_futures_reconciler.h"
#include "providers/binance/binance_parser.h"
#include "providers/binance/binance_rest_client.h"
#include "execution/portfolio.h"
#include "exits/exit_intent.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace {

std::string env_or_empty(const char* name)
{
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string{};
}

// Returns nullptr if either env var is unset (the test should silently
// no-op). Otherwise returns a clock-synced rest client. Tests construct
// it once at the top, return early on null.
std::shared_ptr<BinanceRestClient> setup_or_skip(const char* test_name)
{
    const auto key = env_or_empty("TRUETEST_FUTURES_TESTNET_KEY");
    const auto sec = env_or_empty("TRUETEST_FUTURES_TESTNET_SECRET");
    if (key.empty() || sec.empty())
    {
        std::fprintf(stderr,
            "  (%s: TRUETEST_FUTURES_TESTNET_KEY/SECRET unset, "
            "skipping live testnet round-trip)\n", test_name);
        return nullptr;
    }
    const auto ep = binance::usdm_testnet();
    auto cli = std::make_shared<BinanceRestClient>(
        key, sec, ep.rest_host, ep.rest_port, "/fapi/v1/time");
    if (!cli->resync_clock_now())
    {
        ADD_FAILURE() << "clock resync failed — futures testnet "
                         "unreachable or DNS broken";
        return nullptr;
    }
    return cli;
}

// ms-epoch suffix used to make clientOrderIds and synthetic
// opener_order_ids unique enough that concurrent or repeated test
// runs don't collide.
std::uint64_t unique_id()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace

TEST(BinanceFuturesTestnetLive, PlaceAndCancelLimitOrder)
{
    auto cli = setup_or_skip("BinanceFuturesTestnetLive.PlaceAndCancelLimitOrder");
    if (!cli) return;

    auto info = cli->get_unsigned("/fapi/v1/exchangeInfo", "symbol=BTCUSDT");
    ASSERT_GE(info.status, 200);
    ASSERT_LT(info.status, 300) << "exchangeInfo HTTP " << info.status
                                << ": " << info.body;

    auto pmode = cli->get("/fapi/v1/positionSide/dual", "");
    ASSERT_GE(pmode.status, 200);
    ASSERT_LT(pmode.status, 300) << "positionSide/dual HTTP " << pmode.status
                                 << ": " << pmode.body;
    ASSERT_NE(pmode.body.find("dualSidePosition"), std::string::npos)
        << "positionSide/dual response missing field: " << pmode.body;

    auto px = cli->get_unsigned("/fapi/v1/ticker/price", "symbol=BTCUSDT");
    ASSERT_GE(px.status, 200);
    ASSERT_LT(px.status, 300) << "ticker/price HTTP " << px.status
                              << ": " << px.body;

    double mark = 0.0;
    auto px_sv = binance::extract_sv_string(px.body, "price");
    ASSERT_TRUE(binance::parse_double_sv(px_sv, mark)) << px.body;
    ASSERT_GT(mark, 0.0);

    char price_buf[32];
    std::snprintf(price_buf, sizeof(price_buf), "%.1f", mark * 0.5);

    const std::string client_id = "tt-fut-it-" + std::to_string(unique_id());

    const std::string place_params =
        std::string("symbol=BTCUSDT&side=BUY&type=LIMIT&timeInForce=GTC")
        + "&quantity=0.001&price=" + price_buf
        + "&newClientOrderId=" + client_id;

    auto place = cli->post("/fapi/v1/order", place_params);
    ASSERT_GE(place.status, 200);
    ASSERT_LT(place.status, 300) << "place HTTP " << place.status
                                 << ": " << place.body;

    auto cancel = cli->del(
        "/fapi/v1/order",
        "symbol=BTCUSDT&origClientOrderId=" + client_id);
    EXPECT_GE(cancel.status, 200);
    EXPECT_LT(cancel.status, 300) << "cancel HTTP " << cancel.status
                                  << ": " << cancel.body;
}

TEST(BinanceFuturesTestnetLive, ListenKeyLifecycle)
{
    auto cli = setup_or_skip("BinanceFuturesTestnetLive.ListenKeyLifecycle");
    if (!cli) return;

    // POST creates a fresh listenKey for the user-data stream.
    auto post = cli->post_unsigned("/fapi/v1/listenKey");
    ASSERT_GE(post.status, 200);
    ASSERT_LT(post.status, 300) << "listenKey POST HTTP " << post.status
                                << ": " << post.body;
    auto key = binance::extract_string(post.body, "listenKey");
    ASSERT_FALSE(key.empty()) << "listenKey field missing: " << post.body;

    // PUT is the keepalive — extends the key's TTL without rotating it.
    // The user-data transport's keepalive worker hits this every 30 min.
    auto put = cli->put_unsigned("/fapi/v1/listenKey",
                                  "listenKey=" + key);
    EXPECT_GE(put.status, 200);
    EXPECT_LT(put.status, 300) << "listenKey PUT HTTP " << put.status
                               << ": " << put.body;

    // DELETE closes the user-data stream. Signed del works even though
    // the endpoint accepts unsigned — Binance ignores extra params.
    auto del = cli->del("/fapi/v1/listenKey",
                        "listenKey=" + key);
    EXPECT_GE(del.status, 200);
    EXPECT_LT(del.status, 300) << "listenKey DELETE HTTP " << del.status
                               << ": " << del.body;
}

TEST(BinanceFuturesTestnetLive, BracketAdapterRoundTrip)
{
    auto cli = setup_or_skip("BinanceFuturesTestnetLive.BracketAdapterRoundTrip");
    if (!cli) return;

    auto px = cli->get_unsigned("/fapi/v1/ticker/price", "symbol=BTCUSDT");
    ASSERT_GE(px.status, 200);
    ASSERT_LT(px.status, 300) << px.body;
    double mark = 0.0;
    ASSERT_TRUE(binance::parse_double_sv(
        binance::extract_sv_string(px.body, "price"), mark)) << px.body;

    auto adapter = make_binance_futures_bracket_adapter(cli);

    // Synthetic opener — uniqueness avoids collision with a previous
    // run's orphans on the same testnet account.
    const std::uint64_t opener = unique_id();

    truetest::exits::exit_intent intent;
    intent.symbol         = "BTCUSDT";
    intent.close_side     = order_side::sell;   // pretend we're closing a long
    intent.qty            = 0.001;
    intent.qty_fraction   = 1.0;
    intent.stop_loss      = mark * 0.5;          // far below; can't trigger
    intent.take_profit    = mark * 1.5;          // far above; can't trigger
    intent.opener_order_id = opener;
    intent.strategy_name  = "test";

    auto handles = adapter->place(opener, intent, mark);
    ASSERT_TRUE(handles.sl_exchange_id.has_value())
        << "SL leg did not place — bracket adapter regression";
    ASSERT_TRUE(handles.tp_exchange_id.has_value())
        << "TP leg did not place after SL succeeded";
    EXPECT_EQ(handles.symbol, "BTCUSDT");

    // list_open should round-trip both legs back as a recovered_bracket
    // for our opener. We tolerate other tt-fb- brackets from prior runs.
    auto recovered = adapter->list_open();
    bool found = false;
    for (const auto& r : recovered)
    {
        if (r.opener_order_id == opener)
        {
            EXPECT_EQ(r.symbol, "BTCUSDT");
            EXPECT_TRUE(r.handles.sl_exchange_id.has_value());
            EXPECT_TRUE(r.handles.tp_exchange_id.has_value());
            EXPECT_EQ(r.close_side, order_side::sell);
            ASSERT_TRUE(r.stop_loss.has_value());
            ASSERT_TRUE(r.take_profit.has_value());
            // Allow Binance to round our trigger prices to tick size.
            EXPECT_NEAR(*r.stop_loss,   *intent.stop_loss,   mark * 0.001);
            EXPECT_NEAR(*r.take_profit, *intent.take_profit, mark * 0.001);
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "list_open did not surface our just-placed bracket";

    adapter->cancel(opener, handles);

    auto recovered_after = adapter->list_open();
    bool still_present = false;
    for (const auto& r : recovered_after)
    {
        if (r.opener_order_id == opener) { still_present = true; break; }
    }
    EXPECT_FALSE(still_present) << "bracket survived cancel";
}

TEST(BinanceFuturesTestnetLive, ReconcilerRoundTrip)
{
    auto cli = setup_or_skip("BinanceFuturesTestnetLive.ReconcilerRoundTrip");
    if (!cli) return;

    BinanceFuturesReconciler r(cli, "BTCUSDT", /*is_testnet=*/true);
    portfolio p(0.0);

    // Tolerance is set astronomically high so the test is a pure plumbing
    // check: we want "did the two REST calls succeed and parse cleanly?"
    // not "does an arbitrarily-funded testnet account agree with our
    // empty portfolio?". The drift-detection logic is unit-tested
    // separately via BinanceFuturesReconciler::extract_*.
    auto note = r.reconcile(p, /*tolerance_bps=*/1e9);

    // Either empty (no drift detected even at 1e9 bps — unlikely unless
    // the account is also empty) or a non-empty drift note. Both are
    // sane outcomes for this test; we only fail on REST errors which
    // surface as "BinanceFuturesReconciler: /fapi/v2/... failed (HTTP …)".
    if (!note.empty())
    {
        EXPECT_EQ(note.find("failed (HTTP"), std::string::npos)
            << "REST call inside reconcile() failed: " << note;
    }
    SUCCEED() << "reconcile result: "
              << (note.empty() ? std::string{"(empty — no drift)"} : note);
}

#endif // HAS_BINANCE
