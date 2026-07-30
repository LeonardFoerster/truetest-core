#include <gtest/gtest.h>

// Gated live testnet exercise. Skipped unless TRUETEST_BYBIT_TESTNET_LIVE=1
// and credentials are present. Phase 2: place min-qty limit + cancel.

#ifdef HAS_BYBIT

#include "providers/bybit/bybit_endpoints.h"
#include "providers/bybit/bybit_futures_order_encoder.h"
#include "providers/bybit/bybit_rest_client.h"
#include "providers/bybit/bybit_rest_order_transport.h"
#include "providers/bybit/bybit_time_sync.h"

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>

namespace {

const char* env_or_empty(const char* name)
{
    const char* v = std::getenv(name);
    return v ? v : "";
}

bool live_enabled()
{
    const char* v = std::getenv("TRUETEST_BYBIT_TESTNET_LIVE");
    return v && (v[0] == '1' || v[0] == 't' || v[0] == 'T' || v[0] == 'y'
                 || v[0] == 'Y');
}

} // namespace

TEST(BybitFuturesTestnetLive, PlaceAndCancelLimitOrder)
{
    if (!live_enabled())
        GTEST_SKIP() << "set TRUETEST_BYBIT_TESTNET_LIVE=1 to run";

    const std::string key = env_or_empty("TRUETEST_BYBIT_API_KEY");
    const std::string secret = env_or_empty("TRUETEST_BYBIT_API_SECRET");
    if (key.empty() || secret.empty())
        GTEST_SKIP() << "TRUETEST_BYBIT_API_KEY/SECRET required";

    auto ep = bybit::linear_testnet();
    auto rest = std::make_shared<BybitRestClient>(key, secret, ep);
    rest->set_per_call_timeout(std::chrono::milliseconds(5000));

    ASSERT_TRUE(rest->resync_clock_now()) << "clock resync failed";
    auto skew = bybit::verify_clock_skew(*rest);
    ASSERT_TRUE(skew.ok) << skew.note;

    // Instruments probe for min qty / tick.
    auto info = rest->get_unsigned(
        bybit::paths::instruments_info,
        bybit::instruments_query("linear", "BTCUSDT"));
    ASSERT_TRUE(bybit::is_business_success(info.status, info.body))
        << bybit::truncate_for_log(info.body);
    auto probe = bybit::parse_instruments_response(info.body, "BTCUSDT");
    ASSERT_TRUE(probe.ok) << probe.note;
    ASSERT_GT(probe.spec.lot_size, 0.0);
    ASSERT_GT(probe.spec.tick_size, 0.0);

    // Place a far-from-market limit so it rests; cancel immediately.
    BybitFuturesOrderEncoder enc("BTCUSDT");
    const double qty = probe.spec.min_qty > 0 ? probe.spec.min_qty
                                              : probe.spec.lot_size;
    order_event o(std::chrono::system_clock::now(), "BTCUSDT",
                  order_type::limit, order_side::buy,
                  qty,
                  /*price=*/1000.0, // intentionally far from market
                  time_in_force::gtc);
    o.set_order_id(1);

    const std::string link = "tt-live-test-1";
    auto encoded = enc.encode_submit(o, link);
    ASSERT_FALSE(encoded.endpoint.empty());

    auto tx = make_bybit_rest_order_transport(rest);
    auto placed = tx->submit(encoded.endpoint, encoded.wire_payload);
    ASSERT_TRUE(placed.ok) << placed.error;
    ASSERT_FALSE(placed.exchange_order_id.empty());

    auto cancel_enc =
        enc.encode_cancel("BTCUSDT", placed.exchange_order_id, link);
    auto cancelled = tx->cancel(cancel_enc.endpoint, cancel_enc.wire_payload);
    EXPECT_TRUE(cancelled.ok) << cancelled.error;
}

#endif // HAS_BYBIT
