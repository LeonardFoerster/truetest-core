#include <gtest/gtest.h>

#include "helpers/alloc_counter.h"
#include "orderbook/orderbook.h"
#include "types/control_block_pool.h"

TEST(OrderbookOrderPool, ReplenishUsesPoolNotHeap)
{
    ControlBlockPool cb;
    cb.ensure_min_blocks(1);

    auto ob = std::make_shared<orderbook>();
    ob->configure_order_pool(&cb, 2, true);

    truetest::test::alloc::reset();
    truetest::test::alloc::measure_window window;

    for (int i = 0; i < 64; ++i)
    {
        (void)ob->create_order(ob_order_type::good_till_cancel,
                               static_cast<order_id>(i + 1),
                               side::buy,
                               Price::from_double(100.0 + i),
                               static_cast<quantity>(100));
    }

    const auto snap = window.total();
    EXPECT_LE(snap.count, 5u) << "heap allocs=" << snap.count;
}