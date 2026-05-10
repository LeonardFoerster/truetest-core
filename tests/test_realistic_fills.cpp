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

// Legacy behaviour: market BUY records a single fill at mid × aggression,
// not at the resting ask price the matching engine actually walked.
TEST(RealisticFills, LegacyMarketBuyRecordsAtAggressorPrice)
{
    auto ob = std::make_shared<orderbook>();
    seed_ask(ob, 100.0, 5, 1001);

    LocalBookAdapter adapter(
        ob, /*fee=*/nullptr, /*fill_model=*/nullptr,
        /*rng_seed=*/42, /*aggression=*/1.1, /*qty_scale=*/TEST_QTY_SCALE,
        /*latency=*/nullptr, /*impact=*/nullptr,
        /*realistic_fills=*/false, /*bar_spread_bps=*/0.0);
    adapter.set_mid_price(100.0);

    order_event o(now(), "TEST", order_type::market, order_side::buy, /*qty=*/3.0);
    o.set_order_id(1);
    o.set_earliest_eligible_ts(now());
    adapter.submit_order(o);

    std::vector<fill_event> fills;
    ASSERT_TRUE(adapter.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_NEAR(fills[0].get_fill_price(), 110.0, 0.01)
        << "Legacy: aggressor's marked-up price (mid * 1.1) is recorded";
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
        nullptr, nullptr,
        /*realistic_fills=*/true, /*bar_spread_bps=*/0.0);
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
        nullptr, nullptr,
        /*realistic_fills=*/true, /*bar_spread_bps=*/0.0);
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

// --bar-spread-bps lifts the reference price by half-spread (buy) before
// matching. Without realistic_fills, this surfaces as the recorded fill.
TEST(BarSpread, AppliesHalfSpreadToMarketBuy)
{
    auto ob = std::make_shared<orderbook>();
    // Seeded out at the spread the bar-spread shift will reach. The point
    // of bar_spread is to model a calibrated spread the MM seed didn't
    // include — we seed at +5bps to receive the shifted aggressor.
    seed_ask(ob, 100.05, 10, 1001);

    LocalBookAdapter adapter(
        ob, nullptr, nullptr, 42, /*aggression=*/1.1, TEST_QTY_SCALE,
        nullptr, nullptr,
        /*realistic_fills=*/false, /*bar_spread_bps=*/10.0);  // 10bps full
    adapter.set_mid_price(100.0);

    order_event o(now(), "TEST", order_type::market, order_side::buy, /*qty=*/1.0);
    o.set_order_id(1);
    o.set_earliest_eligible_ts(now());
    adapter.submit_order(o);

    std::vector<fill_event> fills;
    ASSERT_TRUE(adapter.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    // Legacy pricing records aggressor's book_price = ref * aggression
    // where ref = mid * (1 + 5bps) = 100.05 → book_price ≈ 110.055.
    const double expected = 100.0 * (1.0 + 5e-4) * 1.1;
    EXPECT_NEAR(fills[0].get_fill_price(), expected, 0.01);
}

// --bar-spread-bps suppressed when realistic_fills is on — the resting
// walk already incorporates the seeded book's spread.
TEST(BarSpread, SuppressedUnderRealisticFills)
{
    auto ob = std::make_shared<orderbook>();
    seed_ask(ob, 100.05, 10, 1001);

    LocalBookAdapter adapter(
        ob, nullptr, nullptr, 42, /*aggression=*/1.1, TEST_QTY_SCALE,
        nullptr, nullptr,
        /*realistic_fills=*/true, /*bar_spread_bps=*/10.0);
    adapter.set_mid_price(100.0);

    order_event o(now(), "TEST", order_type::market, order_side::buy, /*qty=*/1.0);
    o.set_order_id(1);
    o.set_earliest_eligible_ts(now());
    adapter.submit_order(o);

    std::vector<fill_event> fills;
    ASSERT_TRUE(adapter.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_NEAR(fills[0].get_fill_price(), 100.05, 1e-6)
        << "Resting price only — bar-spread shift suppressed";
}

// --bar-spread-bps suppressed when symbol carries real L2 depth — the
// real seeded book's spread is already correct.
TEST(BarSpread, SuppressedWhenL2Seeded)
{
    auto ob = std::make_shared<orderbook>();
    // Seed at mid (no spread) to make the suppression observable: with
    // suppression off, aggression × shifted-mid would land worse.
    seed_ask(ob, 100.0, 10, 1001);

    LocalBookAdapter adapter(
        ob, nullptr, nullptr, 42, /*aggression=*/1.1, TEST_QTY_SCALE,
        nullptr, nullptr,
        /*realistic_fills=*/false, /*bar_spread_bps=*/10.0);
    adapter.set_mid_price(100.0);
    adapter.set_l2_seeded(true);

    order_event o(now(), "TEST", order_type::market, order_side::buy, /*qty=*/1.0);
    o.set_order_id(1);
    o.set_earliest_eligible_ts(now());
    adapter.submit_order(o);

    std::vector<fill_event> fills;
    ASSERT_TRUE(adapter.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    // Legacy pricing without bar-spread shift: book_price = mid * aggression
    const double expected = 100.0 * 1.1;
    EXPECT_NEAR(fills[0].get_fill_price(), expected, 0.01);
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
        nullptr, nullptr, /*realistic_fills=*/true, 0.0);
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
