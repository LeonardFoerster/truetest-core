#include <gtest/gtest.h>
#include "execution/queue_model.h"

// ---- FrontCancelModel ---------------------------------------------------

TEST(FrontCancelModel, CancelsFullyAheadAdvanceQueue)
{
    FrontCancelModel m;
    // 10 units ahead, a cancel of 4 all came from the front → 6 ahead now.
    EXPECT_DOUBLE_EQ(m.update_on_cancels(10.0, 100.0, 4.0), 6.0);
}

TEST(FrontCancelModel, CancelLargerThanAheadClampsToZero)
{
    FrontCancelModel m;
    // If 50 units cancel and only 10 were ahead, we're now at front.
    EXPECT_DOUBLE_EQ(m.update_on_cancels(10.0, 100.0, 50.0), 0.0);
}

TEST(FrontCancelModel, ZeroCancelsNoop)
{
    FrontCancelModel m;
    EXPECT_DOUBLE_EQ(m.update_on_cancels(10.0, 100.0, 0.0), 10.0);
}

// ---- BackCancelModel ----------------------------------------------------

TEST(BackCancelModel, CancelsNeverAdvanceUs)
{
    BackCancelModel m;
    EXPECT_DOUBLE_EQ(m.update_on_cancels(10.0, 100.0, 50.0), 10.0);
}

TEST(BackCancelModel, ZeroAheadStaysZero)
{
    BackCancelModel m;
    EXPECT_DOUBLE_EQ(m.update_on_cancels(0.0, 100.0, 5.0), 0.0);
}

// ---- UniformCancelModel -------------------------------------------------

TEST(UniformCancelModel, ProportionalToQueueShare)
{
    UniformCancelModel m;
    // Ahead = 25 of a 100-unit level → our share is 0.25.
    // A 20-unit cancel takes 0.25 × 20 = 5 from ahead of us.
    EXPECT_DOUBLE_EQ(m.update_on_cancels(25.0, 100.0, 20.0), 25.0 - 5.0);
}

TEST(UniformCancelModel, FullShareAtBack)
{
    UniformCancelModel m;
    // Fully at the back (size_ahead == total_size) → all cancels hit us.
    EXPECT_DOUBLE_EQ(m.update_on_cancels(100.0, 100.0, 10.0), 90.0);
}

TEST(UniformCancelModel, ZeroTotalSizeIsNoop)
{
    UniformCancelModel m;
    // Defensive: divide-by-zero guard.
    EXPECT_DOUBLE_EQ(m.update_on_cancels(10.0, 0.0, 5.0), 10.0);
}

TEST(UniformCancelModel, ClampsAtZero)
{
    UniformCancelModel m;
    // Contrived: size_ahead 5 of total 10, 100 cancels → 5 - 50 → clamp to 0.
    EXPECT_DOUBLE_EQ(m.update_on_cancels(5.0, 10.0, 100.0), 0.0);
}

// ---- Cross-model invariants --------------------------------------------

TEST(QueueModels, PessimismOrdering)
{
    // For any non-trivial state, Back ≥ Uniform ≥ Front.
    // (Larger size_ahead = more pessimistic about our fill rate.)
    FrontCancelModel   f;
    BackCancelModel    b;
    UniformCancelModel u;

    const double ahead = 30.0, total = 100.0, cancels = 20.0;
    const double r_f = f.update_on_cancels(ahead, total, cancels);
    const double r_b = b.update_on_cancels(ahead, total, cancels);
    const double r_u = u.update_on_cancels(ahead, total, cancels);

    EXPECT_LE(r_f, r_u);
    EXPECT_LE(r_u, r_b);
}
