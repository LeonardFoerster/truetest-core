#include <gtest/gtest.h>
#include "execution/execution_adapter.h"
#include "execution/impact_model.h"

#include <chrono>
#include <cmath>
#include <memory>

TEST(ZeroImpactModel, ReturnsReferencePriceUnchanged)
{
    ZeroImpactModel m;
    EXPECT_DOUBLE_EQ(m.effective_price(order_side::buy,  100.0, 50.0), 50.0);
    EXPECT_DOUBLE_EQ(m.effective_price(order_side::sell, 100.0, 50.0), 50.0);
}

TEST(SquareRootImpactModel, BuysWorseThanReference)
{
    SquareRootImpactModel m(/*k_bps=*/50.0, /*adv=*/10000.0);
    const double px = m.effective_price(order_side::buy, 100.0, 100.0);
    EXPECT_GT(px, 100.0);
}

TEST(SquareRootImpactModel, SellsWorseThanReference)
{
    SquareRootImpactModel m(/*k_bps=*/50.0, /*adv=*/10000.0);
    const double px = m.effective_price(order_side::sell, 100.0, 100.0);
    EXPECT_LT(px, 100.0);
}

TEST(SquareRootImpactModel, LargerOrdersEatMoreImpact)
{
    SquareRootImpactModel m(50.0, 10000.0);
    const double small = m.effective_price(order_side::buy,  10.0, 100.0);
    const double big   = m.effective_price(order_side::buy, 100.0, 100.0);
    EXPECT_GT(big, small);
}

TEST(SquareRootImpactModel, SubLinearGrowth)
{
    SquareRootImpactModel m(50.0, 10000.0);
    // 10× the qty should give √10× the bps, not 10×.
    const double slip1 = m.effective_price(order_side::buy,  10.0, 100.0) - 100.0;
    const double slip10 = m.effective_price(order_side::buy, 100.0, 100.0) - 100.0;
    EXPECT_NEAR(slip10 / slip1, std::sqrt(10.0), 1e-6);
}

TEST(SquareRootImpactModel, ZeroAdvIsNoop)
{
    SquareRootImpactModel m(50.0, 0.0);
    EXPECT_DOUBLE_EQ(m.effective_price(order_side::buy, 100.0, 100.0), 100.0);
}

TEST(SquareRootImpactModel, ZeroQtyIsNoop)
{
    SquareRootImpactModel m(50.0, 10000.0);
    EXPECT_DOUBLE_EQ(m.effective_price(order_side::buy, 0.0, 100.0), 100.0);
}

TEST(SquareRootImpactModel, KnownValue)
{
    // k=50 bps, adv=10000, qty=100 → sqrt(100/10000)=0.1 → impact=5 bps.
    // Buy at reference 200 → 200 * (1 + 5e-4) = 200.1.
    SquareRootImpactModel m(50.0, 10000.0);
    EXPECT_NEAR(m.effective_price(order_side::buy,  100.0, 200.0), 200.1, 1e-9);
    EXPECT_NEAR(m.effective_price(order_side::sell, 100.0, 200.0), 199.9, 1e-9);
}

// ---- Integration with LocalBookAdapter ---------------------------------

namespace {
auto now() { return std::chrono::system_clock::now(); }
}

// Helper: two-level book (ask 100×1, ask 115×10), market buy for qty=2.
// Fills always record at resting prices; impact raises the effective ref
// before aggression and is therefore observable through the crossing
// limit: mid-based limit 100 × 1.1 = 110 reaches only the first level,
// an impact-raised ref ≥ 104.55 pushes the limit past 115 and walks the
// second level too.
static std::vector<fill_event> fills_for_market_buy(
    std::shared_ptr<IImpactModel> impact)
{
    auto ob = std::make_shared<orderbook>();
    ob->add_order(std::make_shared<order>(
        ob_order_type::good_till_cancel, 999, side::sell,
        Price::from_double(100.0), 1));
    ob->add_order(std::make_shared<order>(
        ob_order_type::good_till_cancel, 998, side::sell,
        Price::from_double(115.0), 10));

    LocalBookAdapter adapter(ob, nullptr, nullptr,
                             /*rng*/42, /*agg*/1.1, /*qty_scale*/1.0,
                             /*latency*/nullptr, impact);
    adapter.set_mid_price(100.0);

    order_event o(now(), "X", order_type::market, order_side::buy, 2.0, 100.0);
    o.set_order_id(1);
    o.set_earliest_eligible_ts(now());
    adapter.submit_order(o);

    std::vector<fill_event> fills;
    adapter.poll_fills(fills);
    return fills;
}

TEST(LocalBookAdapterImpact, MarketOrderWithoutImpact_Baseline)
{
    // Sanity: without impact the mid-based crossing limit (110) walks
    // only the first level; the fill records its resting price.
    const auto fills = fills_for_market_buy(nullptr);
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_NEAR(fills[0].get_fill_price(), 100.0, 1e-6);
    EXPECT_NEAR(fills[0].get_filled_quantity(), 1.0, 1e-9);
}

TEST(LocalBookAdapterImpact, MarketOrderWithImpact_WalksDeeperLevel)
{
    // k=500 bps, adv=2, qty=2 → impact = 500 × sqrt(1) = 500 bps = 5%
    // → ref = 105 → limit = 115.5 ≥ 115: the second level is crossed
    // and the order pays the worse resting price there.
    auto impact = std::make_shared<SquareRootImpactModel>(500.0, 2.0);
    const auto fills = fills_for_market_buy(impact);
    ASSERT_EQ(fills.size(), 2u);
    EXPECT_NEAR(fills[0].get_fill_price(), 100.0, 1e-6);
    EXPECT_NEAR(fills[1].get_fill_price(), 115.0, 1e-6);
    EXPECT_NEAR(fills[1].get_filled_quantity(), 1.0, 1e-9);
}

TEST(LocalBookAdapterImpact, MarketOrderWithZeroImpactModel_SameAsNoModel)
{
    // ZeroImpactModel is a pass-through — an adapter configured with it
    // must produce exactly the no-impact fills (back-compat guarantee).
    const auto baseline = fills_for_market_buy(nullptr);
    const auto with_zero = fills_for_market_buy(std::make_shared<ZeroImpactModel>());
    ASSERT_EQ(baseline.size(), with_zero.size());
    for (std::size_t i = 0; i < baseline.size(); ++i)
    {
        EXPECT_NEAR(baseline[i].get_fill_price(),
                    with_zero[i].get_fill_price(), 1e-9);
        EXPECT_NEAR(baseline[i].get_filled_quantity(),
                    with_zero[i].get_filled_quantity(), 1e-9);
    }
}
