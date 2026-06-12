#include <gtest/gtest.h>
#include "execution/execution_adapter.h"

static auto now() { return std::chrono::system_clock::now(); }

// qty_scale=1.0 keeps order_event::quantity 1:1 with book quantity so the
// test arithmetic is readable. Default qty_scale=1e8 would map qty=1.0 to
// 1e8 book units — fine in production, noisy in tests.
static constexpr double TEST_QTY_SCALE = 1.0;

namespace
{
    void seed_ask(const std::shared_ptr<orderbook>& ob, double price, quantity qty,
                  order_id id)
    {
        ob->add_order(std::make_shared<order>(
            ob_order_type::good_till_cancel, id,
            side::sell, Price::from_double(price), qty));
    }
}

// Passive-side pricing is always on: the deprecated realistic_fills=false
// parameter must NOT bring back the legacy mid × aggression fill price.
TEST(RealisticFills, DeprecatedLegacyFlagStillFillsAtRestingPrice)
{
    auto ob = std::make_shared<orderbook>();
    seed_ask(ob, 100.0, 5, 1001);

    LocalBookAdapter adapter(
        ob, /*fee=*/nullptr, /*fill_model=*/nullptr,
        /*rng_seed=*/42, /*aggression=*/1.1, /*qty_scale=*/TEST_QTY_SCALE,
        /*latency=*/nullptr, /*impact=*/nullptr);
    adapter.set_mid_price(100.0);

    order_event o(now(), "TEST", order_type::market, order_side::buy, /*qty=*/3.0);
    o.set_order_id(1);
    o.set_earliest_eligible_ts(now());
    adapter.submit_order(o);

    std::vector<fill_event> fills;
    ASSERT_TRUE(adapter.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_NEAR(fills[0].get_fill_price(), 100.0, 1e-6)
        << "Resting ask price recorded; mid × 1.1 is only the crossing limit";
    EXPECT_NEAR(fills[0].get_filled_quantity(), 3.0, 1e-9);
}

// Realistic behaviour: same market BUY against the same single-level book
// records the resting ask price.
TEST(RealisticFills, MarketBuyRecordsAtRestingAskPrice)
{
    auto ob = std::make_shared<orderbook>();
    seed_ask(ob, 100.0, 5, 1001);

    LocalBookAdapter adapter(
        ob, nullptr, nullptr, 42, /*aggression=*/1.1, TEST_QTY_SCALE,
        nullptr, nullptr);
    adapter.set_mid_price(100.0);

    order_event o(now(), "TEST", order_type::market, order_side::buy, /*qty=*/3.0);
    o.set_order_id(1);
    o.set_earliest_eligible_ts(now());
    adapter.submit_order(o);

    std::vector<fill_event> fills;
    ASSERT_TRUE(adapter.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_NEAR(fills[0].get_fill_price(), 100.0, 1e-6)
        << "Realistic: resting ask price is recorded";
    EXPECT_NEAR(fills[0].get_filled_quantity(), 3.0, 1e-9);
}

// Walking two levels emits two fill_events at their respective resting
// prices — one per matched level head.
TEST(RealisticFills, MarketBuyWalksMultipleLevels)
{
    auto ob = std::make_shared<orderbook>();
    // Whole-number book quantities — qty_scale=1.0 + std::round() in
    // submit_order rounds order_event::quantity to integer book units.
    seed_ask(ob, 100.0, 1, 1001);
    seed_ask(ob, 101.0, 2, 1002);
    seed_ask(ob, 102.0, 5, 1003);

    LocalBookAdapter adapter(
        ob, nullptr, nullptr, 42, /*aggression=*/1.1, TEST_QTY_SCALE,
        nullptr, nullptr);
    adapter.set_mid_price(100.5);

    order_event o(now(), "TEST", order_type::market, order_side::buy, /*qty=*/3.0);
    o.set_order_id(1);
    o.set_earliest_eligible_ts(now());
    adapter.submit_order(o);

    std::vector<fill_event> fills;
    ASSERT_TRUE(adapter.poll_fills(fills));
    ASSERT_EQ(fills.size(), 2u)
        << "One fill_event per walked level (1 @ 100, 2 @ 101)";

    EXPECT_NEAR(fills[0].get_fill_price(),       100.0, 1e-6);
    EXPECT_NEAR(fills[0].get_filled_quantity(),    1.0, 1e-9);
    EXPECT_NEAR(fills[1].get_fill_price(),       101.0, 1e-6);
    EXPECT_NEAR(fills[1].get_filled_quantity(),    2.0, 1e-9);
}

// --bar-spread-bps no longer moves the recorded fill price: the resting
// book is the sole source of spread cost. The deprecated parameter must
// be inert regardless of the (also deprecated) realistic_fills value.
TEST(BarSpread, InertOnRecordedFillPrice)
{
    auto ob = std::make_shared<orderbook>();
    seed_ask(ob, 100.05, 10, 1001);

    LocalBookAdapter adapter(
        ob, nullptr, nullptr, 42, /*aggression=*/1.1, TEST_QTY_SCALE,
        nullptr, nullptr);  // (bar-spread shift no longer exists)
    adapter.set_mid_price(100.0);

    order_event o(now(), "TEST", order_type::market, order_side::buy, /*qty=*/1.0);
    o.set_order_id(1);
    o.set_earliest_eligible_ts(now());
    adapter.submit_order(o);

    std::vector<fill_event> fills;
    ASSERT_TRUE(adapter.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_NEAR(fills[0].get_fill_price(), 100.05, 1e-6)
        << "Resting price only — bar-spread shift removed";
}

// Same inertness with real L2 depth flagged.
TEST(BarSpread, InertWhenL2Seeded)
{
    auto ob = std::make_shared<orderbook>();
    seed_ask(ob, 100.0, 10, 1001);

    LocalBookAdapter adapter(
        ob, nullptr, nullptr, 42, /*aggression=*/1.1, TEST_QTY_SCALE,
        nullptr, nullptr);
    adapter.set_mid_price(100.0);
    adapter.set_l2_seeded(true);

    order_event o(now(), "TEST", order_type::market, order_side::buy, /*qty=*/1.0);
    o.set_order_id(1);
    o.set_earliest_eligible_ts(now());
    adapter.submit_order(o);

    std::vector<fill_event> fills;
    ASSERT_TRUE(adapter.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_NEAR(fills[0].get_fill_price(), 100.0, 1e-6)
        << "Resting price recorded, not mid × aggression";
}

// Sell side mirror: realistic fills on a market SELL records resting bid.
TEST(RealisticFills, MarketSellRecordsAtRestingBidPrice)
{
    auto ob = std::make_shared<orderbook>();
    ob->add_order(std::make_shared<order>(
        ob_order_type::good_till_cancel, 2001,
        side::buy, Price::from_double(99.0), 5));

    LocalBookAdapter adapter(
        ob, nullptr, nullptr, 42, /*aggression=*/1.1, TEST_QTY_SCALE,
        nullptr, nullptr);
    adapter.set_mid_price(99.0);

    order_event o(now(), "TEST", order_type::market, order_side::sell, /*qty=*/2.0);
    o.set_order_id(1);
    o.set_earliest_eligible_ts(now());
    adapter.submit_order(o);

    std::vector<fill_event> fills;
    ASSERT_TRUE(adapter.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_NEAR(fills[0].get_fill_price(), 99.0, 1e-6);
}
