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
    // k=50 bps, adv=10000, qty=100 -> sqrt(100/10000)=0.1 -> impact=5 bps.
    // Buy at reference 200 -> 200 * (1 + 5e-4) = 200.1.
    SquareRootImpactModel m(50.0, 10000.0);
    EXPECT_NEAR(m.effective_price(order_side::buy,  100.0, 200.0), 200.1, 1e-9);
    EXPECT_NEAR(m.effective_price(order_side::sell, 100.0, 200.0), 199.9, 1e-9);
}

// ---- Integration with LocalBookAdapter ---------------------------------

namespace {
auto now() { return std::chrono::system_clock::now(); }
}

// Helper: build a minimal adapter with a single deep ask at 100, submit a
// market buy, and return the fill price. The LocalBookAdapter's market-order
// path builds a marketable limit at mid × aggression (default 1.1 = 110 at
// mid 100) and the orderbook records the fill at that aggressor price.
// Impact raises the effective mid, which raises the book_price in turn, so
// a non-trivial impact model must produce a worse (higher for buys) fill.
static double fill_price_for_market_buy(std::shared_ptr<IImpactModel> impact)
{
    auto ob = std::make_shared<orderbook>();
    ob->add_order(std::make_shared<order>(
        ob_order_type::good_till_cancel, 999, side::sell,
        Price::from_double(100.0), 1'000));

    LocalBookAdapter adapter(ob, nullptr, nullptr,
                             /*rng*/42, /*agg*/1.1, /*qty_scale*/1e8,
                             /*latency*/nullptr, impact);
    adapter.set_mid_price(100.0);

    order_event o(now(), "X", order_type::market, order_side::buy, 10, 100.0);
    o.set_order_id(1);
    o.set_earliest_eligible_ts(now());
    adapter.submit_order(o);

    std::vector<fill_event> fills;
    if (!adapter.poll_fills(fills) || fills.empty()) return 0.0;
    return fills[0].get_fill_price();
}

TEST(LocalBookAdapterImpact, MarketOrderWithoutImpact_Baseline)
{
    // Sanity: without impact the fill price is the aggression-inflated
    // mid (100 × 1.1 = 110). This captures the pre-impact baseline so
    // the comparison test below has a reference.
    const double px = fill_price_for_market_buy(nullptr);
    EXPECT_NEAR(px, 110.0, 0.01);
}

TEST(LocalBookAdapterImpact, MarketOrderWithImpact_PaysWorsePrice)
{
    const double baseline = fill_price_for_market_buy(nullptr);
    // Impact raises ref_price above mid before aggression applies, so
    // the fill should land strictly above baseline.
    auto impact = std::make_shared<SquareRootImpactModel>(500.0, 100.0);
    const double with_impact = fill_price_for_market_buy(impact);
    EXPECT_GT(with_impact, baseline);
}

TEST(LocalBookAdapterImpact, MarketOrderWithZeroImpactModel_SameAsNoModel)
{
    // ZeroImpactModel is a pass-through - an adapter configured with it
    // must produce exactly the no-impact price (back-compat guarantee).
    const double baseline = fill_price_for_market_buy(nullptr);
    auto impact = std::make_shared<ZeroImpactModel>();
    const double with_zero_impact = fill_price_for_market_buy(impact);
    EXPECT_NEAR(baseline, with_zero_impact, 1e-9);
}
