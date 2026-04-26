// Pins the IBracketAdapter contract surface and the NullBracketAdapter
// fallback. Each venue-specific adapter (BinanceOcoBracketAdapter, etc.)
// has its own targeted suite — these tests stay generic.

#include <gtest/gtest.h>

#include "exits/bracket_adapter.h"
#include "exits/exit_intent.h"

using namespace truetest::exits;

TEST(NullBracketAdapter, AdvertisesNoCapabilities)
{
    NullBracketAdapter a;
    auto c = a.capabilities();
    EXPECT_FALSE(c.stop_market);
    EXPECT_FALSE(c.stop_limit);
    EXPECT_FALSE(c.oco);
    EXPECT_FALSE(c.trailing_stop);
}

TEST(NullBracketAdapter, PlaceReturnsEmptyHandlesAndCancelIsNoop)
{
    NullBracketAdapter a;
    exit_intent ei;
    ei.symbol           = "X";
    ei.close_side       = order_side::sell;
    ei.qty              = 1.0;
    ei.stop_loss        = 95.0;
    ei.take_profit      = 110.0;
    ei.opener_order_id  = 42;
    ei.strategy_name    = "s";

    auto h = a.place(42, ei, 100.0);
    EXPECT_TRUE(h.empty());

    // Must tolerate cancel even when nothing was placed.
    EXPECT_NO_THROW(a.cancel(42, h));
    EXPECT_NO_THROW(a.cancel(42, {}));
}

TEST(BracketHandles, EmptyDetection)
{
    bracket_handles h;
    EXPECT_TRUE(h.empty());

    h.sl_exchange_id = "abc";
    EXPECT_FALSE(h.empty());

    h = {};
    h.oco_list_id = "g1";
    EXPECT_FALSE(h.empty());
}
