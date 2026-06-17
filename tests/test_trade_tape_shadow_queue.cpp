// Queue-position behaviour of TradeTapeShadowAdapter - Phase 2A of the
// realism plan. Without the queue model the adapter fills any limit the
// moment a real trade crosses it; with --queue-model l2-snapshot it
// holds the order until the queue ahead has been consumed by prints.

#include <gtest/gtest.h>
#include "execution/trade_tape_shadow_adapter.h"
#include "execution/queue_position_model.h"

#include <chrono>
#include <memory>

using tp = std::chrono::system_clock::time_point;
using ms = std::chrono::milliseconds;

namespace {

order_event limit_buy(uint64_t id, double price, double qty, tp ts,
                      const std::string& sym = "X")
{
    order_event o(ts, sym, order_type::limit, order_side::buy, qty, price,
                  time_in_force::gtc);
    o.set_order_id(id);
    return o;
}

}

// Baseline: snapshot says 5 BTC ahead of us; trade prints below the
// queue (3 BTC) leave 2 ahead, no fill yet. The next print finishes the
// queue and fills our 2 BTC at the trade price.
TEST(TradeTapeShadowQueue, FillsOnlyAfterQueueDrains)
{
    auto qm = std::make_shared<L2SnapshotQueueModel>(ms(60'000));
    const auto t0 = std::chrono::system_clock::now();
    qm->on_snapshot("X", {{100.0, 5.0}}, {});

    TradeTapeShadowAdapter a;
    a.set_queue_model(qm);
    a.submit_order(limit_buy(1, /*price=*/100.0, /*qty=*/2.0, t0));

    std::vector<fill_event> fills;

    // First print: 3 BTC at our level -> consumes 3 of queue -> no fill.
    a.on_trade("X", 100.0, 3.0, t0 + ms(1));
    EXPECT_FALSE(a.poll_fills(fills));

    // Second print: 4 BTC at our level -> consumes the remaining 2 of
    // queue, then fills 2 of ours.
    a.on_trade("X", 100.0, 4.0, t0 + ms(2));
    ASSERT_TRUE(a.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_NEAR(fills[0].get_filled_quantity(), 2.0, 1e-9);
    EXPECT_NEAR(fills[0].get_fill_price(),     100.0, 1e-9);

    const auto qs = a.get_queue_stats();
    EXPECT_EQ(qs.submitted_with_queue, 1u);
    EXPECT_EQ(qs.filled_after_drain,   1u);
    EXPECT_EQ(qs.blocked_at_eos,       0u);
}

// Improving the BBO (price above the best bid -> no level at our price)
// -> queue_ahead = 0 -> first crossing print fills us immediately.
TEST(TradeTapeShadowQueue, ImprovingBBOFillsImmediately)
{
    auto qm = std::make_shared<L2SnapshotQueueModel>(ms(60'000));
    const auto t0 = std::chrono::system_clock::now();
    qm->on_snapshot("X", {{100.0, 5.0}}, {});

    TradeTapeShadowAdapter a;
    a.set_queue_model(qm);
    // Improving: 100.5 sits above the existing best bid of 100.
    a.submit_order(limit_buy(1, /*price=*/100.5, /*qty=*/2.0, t0));

    std::vector<fill_event> fills;
    a.on_trade("X", 100.0, 1.0, t0 + ms(1));
    ASSERT_TRUE(a.poll_fills(fills));
    EXPECT_EQ(fills.size(), 1u);

    // Improver shouldn't be counted as queue-blocked - initial_queue is 0.
    const auto qs = a.get_queue_stats();
    EXPECT_EQ(qs.submitted_with_queue, 0u);
}

// Order remains queue-blocked at session end -> blocked_at_eos counts it.
// "Submitted with queue ahead" + "filled after drain" + "blocked at EOS"
// triangulate where each queued order ended up.
TEST(TradeTapeShadowQueue, QueueBlockedAtSessionEnd)
{
    auto qm = std::make_shared<L2SnapshotQueueModel>(ms(60'000));
    const auto t0 = std::chrono::system_clock::now();
    qm->on_snapshot("X", {{100.0, 100.0}}, {});  // huge queue ahead

    TradeTapeShadowAdapter a;
    a.set_queue_model(qm);
    a.submit_order(limit_buy(1, /*price=*/100.0, /*qty=*/2.0, t0));

    a.on_trade("X", 100.0, 5.0, t0 + ms(1));  // chips at queue, no fill
    std::vector<fill_event> fills;
    EXPECT_FALSE(a.poll_fills(fills));

    const auto qs = a.get_queue_stats();
    EXPECT_EQ(qs.submitted_with_queue, 1u);
    EXPECT_EQ(qs.filled_after_drain,   0u);
    EXPECT_EQ(qs.blocked_at_eos,       1u);
}

// Determinism guarantee: with no queue model set, behaviour is exactly
// the legacy fill-on-cross. Existing trade-tape tests rely on this.
TEST(TradeTapeShadowQueue, NoQueueModelPreservesLegacy)
{
    const auto t0 = std::chrono::system_clock::now();
    TradeTapeShadowAdapter a;
    a.submit_order(limit_buy(1, /*price=*/100.0, /*qty=*/2.0, t0));
    a.on_trade("X", 100.0, 1.0, t0 + ms(1));

    std::vector<fill_event> fills;
    ASSERT_TRUE(a.poll_fills(fills));
    EXPECT_EQ(fills.size(), 1u);
    EXPECT_NEAR(fills[0].get_filled_quantity(), 1.0, 1e-9);

    const auto qs = a.get_queue_stats();
    EXPECT_EQ(qs.submitted_with_queue, 0u);
    EXPECT_EQ(qs.filled_after_drain,   0u);
    EXPECT_EQ(qs.blocked_at_eos,       0u);
}

// Equivalent: explicit NoQueueModel must behave identically to no model.
TEST(TradeTapeShadowQueue, NoQueueModelInstancePreservesLegacy)
{
    auto qm = std::make_shared<NoQueueModel>();
    const auto t0 = std::chrono::system_clock::now();
    TradeTapeShadowAdapter a;
    a.set_queue_model(qm);
    a.submit_order(limit_buy(1, 100.0, 2.0, t0));
    a.on_trade("X", 100.0, 1.0, t0 + ms(1));

    std::vector<fill_event> fills;
    ASSERT_TRUE(a.poll_fills(fills));
    EXPECT_EQ(fills.size(), 1u);
}
