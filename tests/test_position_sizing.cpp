#include <gtest/gtest.h>
#include "execution/position_sizing.h"

using namespace truetest::risk;

TEST(PositionSizing, ZeroCostMatchesClassicFixedRisk)
{
    risk_size_inputs in;
    in.equity        = 10000.0;
    in.risk_fraction = 0.02;
    in.entry_price   = 100.0;
    in.stop_price    = 98.0; // $2 risk per unit
    in.is_long       = true;

    // Classic: 200 / 2 = 100
    EXPECT_DOUBLE_EQ(compute_risk_quantity(in), 100.0);
    EXPECT_DOUBLE_EQ(estimate_stop_loss(in, 100.0), 200.0);
}

TEST(PositionSizing, ProportionalFeesShrinkQtyToKeepBudget)
{
    risk_size_inputs in;
    in.equity         = 10000.0;
    in.risk_fraction  = 0.02; // $200 budget
    in.entry_price    = 100.0;
    in.stop_price     = 98.0;
    in.is_long        = true;
    in.entry_fee_rate = 0.001; // 10 bps
    in.exit_fee_rate  = 0.001;

    const double qty = compute_risk_quantity(in);
    ASSERT_GT(qty, 0.0);
    ASSERT_LT(qty, 100.0); // must be smaller than zero-cost size

    const double loss = estimate_stop_loss(in, qty);
    EXPECT_NEAR(loss, 200.0, 1e-9);
}

TEST(PositionSizing, AdverseSlipShrinksQtyToKeepBudget)
{
    risk_size_inputs in;
    in.equity         = 10000.0;
    in.risk_fraction  = 0.02;
    in.entry_price    = 100.0;
    in.stop_price     = 98.0;
    in.is_long        = true;
    in.entry_slip_bps = 10.0; // 10 bps adverse entry
    in.exit_slip_bps  = 10.0;

    const double qty = compute_risk_quantity(in);
    ASSERT_GT(qty, 0.0);
    ASSERT_LT(qty, 100.0);

    EXPECT_NEAR(estimate_stop_loss(in, qty), 200.0, 1e-6);
}

TEST(PositionSizing, FeesAndSlipTogetherHonorBudget)
{
    risk_size_inputs in;
    in.equity         = 10000.0;
    in.risk_fraction  = 0.02;
    in.entry_price    = 100.0;
    in.stop_price     = 99.0; // tight 1% stop
    in.is_long        = true;
    in.entry_fee_rate = 0.0004;
    in.exit_fee_rate  = 0.0004;
    in.entry_slip_bps = 5.0;
    in.exit_slip_bps  = 5.0;

    const double qty = compute_risk_quantity(in);
    ASSERT_GT(qty, 0.0);
    // Zero-cost would be 200 / 1 = 200; with costs must be lower.
    EXPECT_LT(qty, 200.0);
    EXPECT_NEAR(estimate_stop_loss(in, qty), 200.0, 1e-6);
}

TEST(PositionSizing, FixedFeePerLegHonorsBudget)
{
    risk_size_inputs in;
    in.equity            = 10000.0;
    in.risk_fraction     = 0.02; // $200
    in.entry_price       = 100.0;
    in.stop_price        = 98.0;
    in.is_long           = true;
    in.fixed_fee_per_leg = 5.0; // $10 round-trip

    const double qty = compute_risk_quantity(in);
    // (200 - 10) / 2 = 95
    EXPECT_DOUBLE_EQ(qty, 95.0);
    EXPECT_NEAR(estimate_stop_loss(in, qty), 200.0, 1e-9);
}

TEST(PositionSizing, ShortSideSymmetric)
{
    risk_size_inputs in;
    in.equity         = 10000.0;
    in.risk_fraction  = 0.02;
    in.entry_price    = 100.0;
    in.stop_price     = 102.0;
    in.is_long        = false;
    in.entry_fee_rate = 0.0004;
    in.exit_fee_rate  = 0.0004;
    in.entry_slip_bps = 5.0;
    in.exit_slip_bps  = 5.0;

    const double qty = compute_risk_quantity(in);
    ASSERT_GT(qty, 0.0);
    EXPECT_NEAR(estimate_stop_loss(in, qty), 200.0, 1e-6);
}

TEST(PositionSizing, MaxNotionalFracCapsSize)
{
    risk_size_inputs in;
    in.equity            = 10000.0;
    in.risk_fraction     = 0.02; // would size ~100 units without cap
    in.entry_price       = 100.0;
    in.stop_price        = 98.0;
    in.is_long           = true;
    in.max_notional_frac = 0.05; // max $500 notional → 5 units

    EXPECT_NEAR(compute_risk_quantity(in), 5.0, 1e-9);
}

TEST(PositionSizing, NotionalSizingAccountsForEntryFee)
{
    // Deploy 2% of 10k = $200 notional budget including fee.
    const double qty = compute_notional_quantity(
        10000.0, 0.02, 100.0, /*entry_fee_rate=*/0.001);
    // unit_cost = 100 * 1.001 = 100.1 → qty = 200/100.1
    EXPECT_NEAR(qty, 200.0 / 100.1, 1e-9);
}

TEST(PositionSizing, DegenerateStopDoesNotInfiniteSize)
{
    risk_size_inputs in;
    in.equity        = 10000.0;
    in.risk_fraction = 0.02;
    in.entry_price   = 100.0;
    in.stop_price    = 100.0; // zero distance
    in.is_long       = true;

    const double qty = compute_risk_quantity(in);
    ASSERT_GT(qty, 0.0);
    ASSERT_LT(qty, 1e6);
    EXPECT_NEAR(estimate_stop_loss(in, qty), 200.0, 1e-6);
}
