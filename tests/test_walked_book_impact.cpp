// Walked-book impact: when L2 depth is real, market orders price against
// the actual VWAP they'd consume rather than mid + parametric impact.
// Fills always record at the resting counterparty's price, so the walked
// VWAP is observable only through the aggressor's crossing limit
// (ref × aggression): a higher ref lets the order reach deeper levels
// when the cheap depth can't cover the quantity. Only activates when
// l2_seeded_ is set.

#include <gtest/gtest.h>
#include "execution/execution_adapter.h"
#include "execution/impact_model.h"

static auto now() { return std::chrono::system_clock::now(); }
static constexpr double TEST_QTY_SCALE = 1.0;

namespace
{
    void seed_ask(const std::shared_ptr<orderbook>& ob, double price,
                  quantity qty, order_id id)
    {
        ob->add_order(std::make_shared<order>(
            ob_order_type::good_till_cancel, id,
            side::sell, Price::from_double(price), qty));
    }

    void seed_bid(const std::shared_ptr<orderbook>& ob, double price,
                  quantity qty, order_id id)
    {
        ob->add_order(std::make_shared<order>(
            ob_order_type::good_till_cancel, id,
            side::buy, Price::from_double(price), qty));
    }
}

// Walked-book on: asks 100×1, 120×2, qty=2 → vwap = 110 → crossing limit
// 110 × 1.1 = 121 reaches the 120 level. Without the walk, mid-based
// limit 100 × 1.1 = 110 would cross only the first level. Fills record
// the resting prices of both walked levels.
TEST(WalkedBookImpact, MarketBuyVWAPExtendsCrossingLimit)
{
    auto ob = std::make_shared<orderbook>();
    seed_ask(ob, 100.0, 1, 1001);
    seed_ask(ob, 120.0, 2, 1002);

    LocalBookAdapter adapter(
        ob, nullptr, nullptr, 42, /*aggression=*/1.1, TEST_QTY_SCALE,
        nullptr, nullptr,
        /*realistic_fills(deprecated)=*/false, /*bar_spread_bps=*/0.0,
        /*walked_book_impact=*/true);
    adapter.set_mid_price(100.0);
    adapter.set_l2_seeded(true);

    order_event o(now(), "TEST", order_type::market, order_side::buy, /*qty=*/2.0);
    o.set_order_id(1);
    o.set_earliest_eligible_ts(now());
    adapter.submit_order(o);

    std::vector<fill_event> fills;
    ASSERT_TRUE(adapter.poll_fills(fills));
    ASSERT_EQ(fills.size(), 2u)
        << "VWAP-based crossing limit must reach the 120 level";
    EXPECT_NEAR(fills[0].get_fill_price(),       100.0, 1e-6);
    EXPECT_NEAR(fills[0].get_filled_quantity(),    1.0, 1e-9);
    EXPECT_NEAR(fills[1].get_fill_price(),       120.0, 1e-6);
    EXPECT_NEAR(fills[1].get_filled_quantity(),    1.0, 1e-9);
}

// Sell-side mirror: bids 99×3, 88×5, qty=4 → vwap = (99×3 + 88)/4 = 96.25
// → crossing limit 96.25 × 0.9 = 86.625 reaches the 88 level. Mid-based
// limit 99 × 0.9 = 89.1 would not (88 < 89.1).
TEST(WalkedBookImpact, MarketSellVWAPExtendsCrossingLimit)
{
    auto ob = std::make_shared<orderbook>();
    seed_bid(ob, 99.0, 3, 2001);
    seed_bid(ob, 88.0, 5, 2002);

    LocalBookAdapter adapter(
        ob, nullptr, nullptr, 42, 1.1, TEST_QTY_SCALE,
        nullptr, nullptr, /*realistic_fills(deprecated)=*/false, 0.0,
        /*walked_book_impact=*/true);
    adapter.set_mid_price(99.0);
    adapter.set_l2_seeded(true);

    order_event o(now(), "TEST", order_type::market, order_side::sell, /*qty=*/4.0);
    o.set_order_id(1);
    o.set_earliest_eligible_ts(now());
    adapter.submit_order(o);

    std::vector<fill_event> fills;
    ASSERT_TRUE(adapter.poll_fills(fills));
    ASSERT_EQ(fills.size(), 2u)
        << "VWAP-based crossing limit must reach the 88 level";
    EXPECT_NEAR(fills[0].get_fill_price(),       99.0, 1e-6);
    EXPECT_NEAR(fills[0].get_filled_quantity(),   3.0, 1e-9);
    EXPECT_NEAR(fills[1].get_fill_price(),       88.0, 1e-6);
    EXPECT_NEAR(fills[1].get_filled_quantity(),   1.0, 1e-9);
}

// Insufficient depth → walked VWAP returns 0 → falls back to mid (plus
// impact_model when present), NOT to the partial-walk VWAP. Partial-walk
// vwap (100 + 3×115)/4 = 111.25 would push the crossing limit to 122 and
// reach the 115 level; honest mid fallback (limit 110) must not.
TEST(WalkedBookImpact, InsufficientDepthFallsBackToMid)
{
    auto ob = std::make_shared<orderbook>();
    seed_ask(ob, 100.0, 1, 1001);
    seed_ask(ob, 115.0, 3, 1002);  // total depth 4 < qty 5

    LocalBookAdapter adapter(
        ob, nullptr, nullptr, 42, 1.1, TEST_QTY_SCALE,
        nullptr, nullptr, false, 0.0,
        /*walked_book_impact=*/true);
    adapter.set_mid_price(100.0);
    adapter.set_l2_seeded(true);

    order_event o(now(), "TEST", order_type::market, order_side::buy, /*qty=*/5.0);
    o.set_order_id(1);
    o.set_earliest_eligible_ts(now());
    adapter.submit_order(o);

    std::vector<fill_event> fills;
    ASSERT_TRUE(adapter.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u)
        << "Mid-based limit 110 crosses only the 100 level";
    EXPECT_NEAR(fills[0].get_fill_price(), 100.0, 1e-6);
    EXPECT_NEAR(fills[0].get_filled_quantity(), 1.0, 1e-9);
}

// Walked-book suppresses the parametric impact model when both are
// configured — they'd double-count slippage on the same depth. asks
// 100×2, 120×5, qty=3 → vwap = 320/3 ≈ 106.67 → limit ≈ 117.3 < 120:
// second level not crossed. A huge square-root impact stacked on top
// (20% → limit ≈ 140) would cross it.
TEST(WalkedBookImpact, SuppressesSquareRootWhenBothActive)
{
    auto ob = std::make_shared<orderbook>();
    seed_ask(ob, 100.0, 2, 1001);
    seed_ask(ob, 120.0, 5, 1002);

    auto sqrt_impact = std::make_shared<SquareRootImpactModel>(
        /*k_bps=*/2000.0, /*adv=*/3.0);  // 20% impact at qty=3 if applied

    LocalBookAdapter adapter(
        ob, nullptr, nullptr, 42, 1.1, TEST_QTY_SCALE,
        nullptr, sqrt_impact,
        false, 0.0, /*walked_book_impact=*/true);
    adapter.set_mid_price(100.0);
    adapter.set_l2_seeded(true);

    order_event o(now(), "TEST", order_type::market, order_side::buy, /*qty=*/3.0);
    o.set_order_id(1);
    o.set_earliest_eligible_ts(now());
    adapter.submit_order(o);

    std::vector<fill_event> fills;
    ASSERT_TRUE(adapter.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u)
        << "Impact suppressed: VWAP limit 117.3 must not reach 120";
    EXPECT_NEAR(fills[0].get_fill_price(), 100.0, 1e-6);
    EXPECT_NEAR(fills[0].get_filled_quantity(), 2.0, 1e-9);
}

// No L2 seeded → flag has no effect → mid-based crossing limit. Guards
// against the flag silently changing non-shadow behaviour.
TEST(WalkedBookImpact, NoL2NoEffect)
{
    auto ob = std::make_shared<orderbook>();
    seed_ask(ob, 100.0, 1, 1001);
    seed_ask(ob, 115.0, 5, 1002);  // beyond mid × 1.1; walk would reach it

    LocalBookAdapter adapter(
        ob, nullptr, nullptr, 42, 1.1, TEST_QTY_SCALE,
        nullptr, nullptr, false, 0.0,
        /*walked_book_impact=*/true);
    adapter.set_mid_price(100.0);
    // Note: no set_l2_seeded(true) — symbol is not L2-seeded.

    order_event o(now(), "TEST", order_type::market, order_side::buy, /*qty=*/2.0);
    o.set_order_id(1);
    o.set_earliest_eligible_ts(now());
    adapter.submit_order(o);

    std::vector<fill_event> fills;
    ASSERT_TRUE(adapter.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u)
        << "Mid-based limit 110 must not reach the 115 level";
    EXPECT_NEAR(fills[0].get_fill_price(), 100.0, 1e-6);
}

// Walked-book off → mid-based crossing limit even with L2 present. The
// flag must default to no-op.
TEST(WalkedBookImpact, DefaultOffUsesMidReference)
{
    auto ob = std::make_shared<orderbook>();
    seed_ask(ob, 100.0, 1, 1001);
    seed_ask(ob, 115.0, 5, 1002);

    LocalBookAdapter adapter(
        ob, nullptr, nullptr, 42, 1.1, TEST_QTY_SCALE,
        nullptr, nullptr, false, 0.0,
        /*walked_book_impact=*/false);
    adapter.set_mid_price(100.0);
    adapter.set_l2_seeded(true);  // L2 present, flag off

    order_event o(now(), "TEST", order_type::market, order_side::buy, /*qty=*/2.0);
    o.set_order_id(1);
    o.set_earliest_eligible_ts(now());
    adapter.submit_order(o);

    std::vector<fill_event> fills;
    ASSERT_TRUE(adapter.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_NEAR(fills[0].get_fill_price(), 100.0, 1e-6);
}

// Per-level resting-price fills dictate the recorded prices in all
// configurations; walked-book ref_price affects only the aggressor's
// crossing limit. Both levels here sit inside the limit, so both fills
// emit at their resting prices.
TEST(WalkedBookImpact, FillsRecordRestingPricesPerLevel)
{
    auto ob = std::make_shared<orderbook>();
    seed_ask(ob, 100.0, 1, 1001);
    seed_ask(ob, 101.0, 2, 1002);

    LocalBookAdapter adapter(
        ob, nullptr, nullptr, 42, 1.1, TEST_QTY_SCALE,
        nullptr, nullptr,
        /*realistic_fills(deprecated)=*/true, 0.0,
        /*walked_book_impact=*/true);
    adapter.set_mid_price(100.5);
    adapter.set_l2_seeded(true);

    order_event o(now(), "TEST", order_type::market, order_side::buy, /*qty=*/2.0);
    o.set_order_id(1);
    o.set_earliest_eligible_ts(now());
    adapter.submit_order(o);

    std::vector<fill_event> fills;
    ASSERT_TRUE(adapter.poll_fills(fills));
    ASSERT_EQ(fills.size(), 2u);
    EXPECT_NEAR(fills[0].get_fill_price(), 100.0, 1e-6);
    EXPECT_NEAR(fills[1].get_fill_price(), 101.0, 1e-6);
}
