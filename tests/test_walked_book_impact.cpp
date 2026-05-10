// Walked-book impact: when L2 depth is real, market orders price against
// the actual VWAP they'd consume rather than mid + parametric impact.
// Phase 4 of the realism plan. Composes with --realistic-fills (no
// effect there — the resting walk already prices fills) and only
// activates when l2_seeded_ is set.

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

// Walked-book on: market BUY records aggressor's book_price = vwap *
// aggression. With ask levels 100×1, 101×2 and qty=2, vwap = 100.5.
TEST(WalkedBookImpact, MarketBuyUsesVWAPOfWalkedAsks)
{
    auto ob = std::make_shared<orderbook>();
    seed_ask(ob, 100.0, 1, 1001);
    seed_ask(ob, 101.0, 2, 1002);

    LocalBookAdapter adapter(
        ob, nullptr, nullptr, 42, /*aggression=*/1.1, TEST_QTY_SCALE,
        nullptr, nullptr,
        /*realistic_fills=*/false, /*bar_spread_bps=*/0.0,
        /*walked_book_impact=*/true);
    adapter.set_mid_price(100.0);
    adapter.set_l2_seeded(true);

    order_event o(now(), "TEST", order_type::market, order_side::buy, /*qty=*/2.0);
    o.set_order_id(1);
    o.set_earliest_eligible_ts(now());
    adapter.submit_order(o);

    std::vector<fill_event> fills;
    ASSERT_TRUE(adapter.poll_fills(fills));
    // Without realistic_fills, the recorded fill price is the aggressor's
    // book_price = vwap × aggression. (100 + 2×101)/3 = 100.667 → wait,
    // qty is 2 not 3: take 1 @ 100, 1 @ 101 → vwap = (100 + 101)/2 = 100.5.
    const double expected = 100.5 * 1.1;
    // Two fills at the same book_price (matching engine emits one per
    // matched level, both at the aggressor's submitted price under
    // legacy fill-pricing).
    ASSERT_GE(fills.size(), 1u);
    EXPECT_NEAR(fills.back().get_fill_price(), expected, 0.05);
}

// Sell-side mirror: walked VWAP for SELL through bids.
TEST(WalkedBookImpact, MarketSellUsesVWAPOfWalkedBids)
{
    auto ob = std::make_shared<orderbook>();
    seed_bid(ob, 99.0, 3, 2001);
    seed_bid(ob, 98.0, 5, 2002);

    LocalBookAdapter adapter(
        ob, nullptr, nullptr, 42, 1.1, TEST_QTY_SCALE,
        nullptr, nullptr, /*realistic_fills=*/false, 0.0,
        /*walked_book_impact=*/true);
    adapter.set_mid_price(99.0);
    adapter.set_l2_seeded(true);

    order_event o(now(), "TEST", order_type::market, order_side::sell, /*qty=*/4.0);
    o.set_order_id(1);
    o.set_earliest_eligible_ts(now());
    adapter.submit_order(o);

    std::vector<fill_event> fills;
    ASSERT_TRUE(adapter.poll_fills(fills));
    // (99×3 + 98×1)/4 = 98.75. SELL aggression: ref × (2 - aggression) =
    // 98.75 × 0.9 = 88.875.
    const double expected = 98.75 * (2.0 - 1.1);
    EXPECT_NEAR(fills.back().get_fill_price(), expected, 0.05);
}

// Insufficient depth → walked returns 0 → falls back to mid + impact_model.
// Without an impact model the mid + aggression baseline applies, NOT
// walked-VWAP of the partial book (that would understate the sweep).
TEST(WalkedBookImpact, InsufficientDepthFallsBackToMid)
{
    auto ob = std::make_shared<orderbook>();
    seed_ask(ob, 100.0, 1, 1001);  // only 1 unit on the book

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
    // Fall-back: mid × aggression = 110. Recorded fill = 110, NOT the
    // partial-walk VWAP that would have been 100.
    EXPECT_NEAR(fills[0].get_fill_price(), 110.0, 0.05);
}

// Walked-book suppresses the parametric impact model when both are
// configured — they'd double-count slippage on the same depth.
TEST(WalkedBookImpact, SuppressesSquareRootWhenBothActive)
{
    auto ob = std::make_shared<orderbook>();
    seed_ask(ob, 100.0, 10, 1001);

    auto sqrt_impact = std::make_shared<SquareRootImpactModel>(
        /*k_bps=*/100.0, /*adv=*/1000.0);  // big k so divergence is obvious

    LocalBookAdapter adapter(
        ob, nullptr, nullptr, 42, 1.1, TEST_QTY_SCALE,
        nullptr, sqrt_impact,
        false, 0.0, /*walked_book_impact=*/true);
    adapter.set_mid_price(100.0);
    adapter.set_l2_seeded(true);

    order_event o(now(), "TEST", order_type::market, order_side::buy, /*qty=*/1.0);
    o.set_order_id(1);
    o.set_earliest_eligible_ts(now());
    adapter.submit_order(o);

    std::vector<fill_event> fills;
    ASSERT_TRUE(adapter.poll_fills(fills));
    // Walked VWAP for qty=1 at ask 100×10 is exactly 100. Aggression
    // gives 110. SquareRoot would have added k·sqrt(qty/adv) = 100·
    // sqrt(1/1000) ≈ 3.16 bps to the ref before aggression, producing
    // ~110.348. Suppressed → 110.0.
    EXPECT_NEAR(fills[0].get_fill_price(), 110.0, 0.05);
}

// No L2 seeded → flag has no effect → falls back to mid + impact_model.
// Guards against the flag silently changing non-shadow behaviour.
TEST(WalkedBookImpact, NoL2NoEffect)
{
    auto ob = std::make_shared<orderbook>();
    seed_ask(ob, 100.0, 5, 1001);

    LocalBookAdapter adapter(
        ob, nullptr, nullptr, 42, 1.1, TEST_QTY_SCALE,
        nullptr, nullptr, false, 0.0,
        /*walked_book_impact=*/true);
    adapter.set_mid_price(100.0);
    // Note: no set_l2_seeded(true) — symbol is not L2-seeded.

    order_event o(now(), "TEST", order_type::market, order_side::buy, /*qty=*/1.0);
    o.set_order_id(1);
    o.set_earliest_eligible_ts(now());
    adapter.submit_order(o);

    std::vector<fill_event> fills;
    ASSERT_TRUE(adapter.poll_fills(fills));
    // Legacy mid × aggression — flag inactive without L2.
    EXPECT_NEAR(fills[0].get_fill_price(), 110.0, 0.05);
}

// Walked-book off → byte-identity to step-1-baseline behaviour with
// realistic_fills also off. The flag must default to no-op for back-
// compat with existing backtests.
TEST(WalkedBookImpact, DefaultOffPreservesLegacy)
{
    auto ob = std::make_shared<orderbook>();
    seed_ask(ob, 100.0, 5, 1001);

    LocalBookAdapter adapter(
        ob, nullptr, nullptr, 42, 1.1, TEST_QTY_SCALE,
        nullptr, nullptr, false, 0.0,
        /*walked_book_impact=*/false);
    adapter.set_mid_price(100.0);
    adapter.set_l2_seeded(true);  // L2 present, flag off

    order_event o(now(), "TEST", order_type::market, order_side::buy, /*qty=*/1.0);
    o.set_order_id(1);
    o.set_earliest_eligible_ts(now());
    adapter.submit_order(o);

    std::vector<fill_event> fills;
    ASSERT_TRUE(adapter.poll_fills(fills));
    // Legacy mid × aggression = 110.
    EXPECT_NEAR(fills[0].get_fill_price(), 110.0, 0.05);
}

// Compose with realistic-fills: the per-level resting walk dictates
// fill prices (Phase 1 behaviour), so walked-book ref_price affects
// only the aggressor's crossing limit. Fills still emit at resting
// prices regardless.
TEST(WalkedBookImpact, ComposesWithRealisticFills)
{
    auto ob = std::make_shared<orderbook>();
    seed_ask(ob, 100.0, 1, 1001);
    seed_ask(ob, 101.0, 2, 1002);

    LocalBookAdapter adapter(
        ob, nullptr, nullptr, 42, 1.1, TEST_QTY_SCALE,
        nullptr, nullptr,
        /*realistic_fills=*/true, 0.0,
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
