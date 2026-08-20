// R3 Phase 1 — authoritative order ledger.
//
// Covers the lifecycle matrix the risk path depends on: open-order count and
// remaining quantity after every state transition, partial fills, terminal
// states, duplicate fills, cancel-after-partial, and per-symbol pending
// exposure. See docs/internal/r3-authoritative-risk-accounting.md.

#include <gtest/gtest.h>

#include "execution/order_tracker.h"

#include <chrono>
#include <string>

namespace {

auto epoch_ms(std::int64_t ms)
{
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

order_event make_order(std::uint64_t id, const std::string& symbol,
                       order_side side, double qty, double price = 100.0,
                       std::int64_t ts_ms = 0)
{
    order_event o(epoch_ms(ts_ms), symbol, order_type::limit, side, qty, price);
    o.set_order_id(id);
    return o;
}

fill_event make_fill(std::uint64_t order_id, const std::string& symbol,
                     order_side side, double qty, double remaining,
                     std::uint64_t fill_id = 0, std::int64_t ts_ms = 0)
{
    return fill_event(epoch_ms(ts_ms), symbol, order_id, side, qty, 100.0, 0.0,
                      remaining, fill_id);
}

// Submits + acknowledges one order and returns the ledger to the caller.
void submit_and_ack(OrderTracker& ledger, const order_event& o)
{
    ledger.register_order(o);
    ledger.set_status(o.get_order_id(), order_status::pending);
    ledger.set_status(o.get_order_id(), order_status::open);
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle matrix
// ---------------------------------------------------------------------------

TEST(OrderLedger, SubmitAckFill)
{
    OrderTracker ledger;
    const auto o = make_order(1, "BTCUSDT", order_side::buy, 5.0);

    ledger.register_order(o);
    EXPECT_EQ(ledger.get_order_status(1), order_status::unknown);
    EXPECT_EQ(ledger.active_count(), 0u);

    ledger.set_status(1, order_status::pending);
    EXPECT_EQ(ledger.active_count(), 1u);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 5.0);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_buy_qty, 5.0);

    ledger.set_status(1, order_status::open);
    EXPECT_EQ(ledger.active_count(), 1u);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_buy_qty, 5.0);

    ledger.on_fill(make_fill(1, "BTCUSDT", order_side::buy, 5.0, 0.0));
    EXPECT_EQ(ledger.get_order_status(1), order_status::filled);
    EXPECT_EQ(ledger.active_count(), 0u);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 0.0);
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 5.0);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_buy_qty, 0.0);
    EXPECT_EQ(ledger.open_exposure("BTCUSDT").open_order_count, 0u);
}

TEST(OrderLedger, SubmitAckPartialFillThenFill)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 10.0));

    ledger.on_fill(make_fill(1, "BTCUSDT", order_side::buy, 4.0, 6.0));
    EXPECT_EQ(ledger.get_order_status(1), order_status::partially_filled);
    EXPECT_EQ(ledger.active_count(), 1u);
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 4.0);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 6.0);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_buy_qty, 6.0);

    ledger.on_fill(make_fill(1, "BTCUSDT", order_side::buy, 6.0, 0.0));
    EXPECT_EQ(ledger.get_order_status(1), order_status::filled);
    EXPECT_EQ(ledger.active_count(), 0u);
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 10.0);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 0.0);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_buy_qty, 0.0);
}

TEST(OrderLedger, SubmitAckPartialFillThenCancelReleasesOnlyTheRemainder)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::sell, 10.0));

    ledger.on_fill(make_fill(1, "BTCUSDT", order_side::sell, 3.0, 7.0));
    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_sell_qty, 7.0);

    ledger.set_status(1, order_status::cancelled);
    EXPECT_EQ(ledger.get_order_status(1), order_status::cancelled);
    EXPECT_EQ(ledger.active_count(), 0u);
    // The filled 3.0 became a position (portfolio's job); only the unfilled
    // remainder leaves pending exposure.
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 3.0);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 0.0);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_sell_qty, 0.0);
    EXPECT_EQ(ledger.open_exposure("BTCUSDT").open_order_count, 0u);
}

TEST(OrderLedger, RejectCarriesNoOpenStateOrExposure)
{
    OrderTracker ledger;
    const auto o = make_order(7, "ETHUSDT", order_side::buy, 2.0);
    ledger.register_order(o);
    ledger.set_status(7, order_status::pending);
    EXPECT_EQ(ledger.active_count(), 1u);

    ledger.set_status(7, order_status::rejected);
    EXPECT_EQ(ledger.get_order_status(7), order_status::rejected);
    EXPECT_TRUE(order_status_is_terminal(order_status::rejected));
    EXPECT_EQ(ledger.active_count(), 0u);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(7), 0.0);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("ETHUSDT").open_buy_qty, 0.0);
}

TEST(OrderLedger, ExpireIsTerminalAndDistinctFromCancel)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(9, "ETHUSDT", order_side::buy, 4.0));
    EXPECT_EQ(ledger.active_count(), 1u);

    ledger.set_status(9, order_status::expired);
    EXPECT_EQ(ledger.get_order_status(9), order_status::expired);
    EXPECT_NE(ledger.get_order_status(9), order_status::cancelled);
    EXPECT_TRUE(order_status_is_terminal(order_status::expired));
    EXPECT_FALSE(order_status_is_open(order_status::expired));
    EXPECT_EQ(ledger.active_count(), 0u);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("ETHUSDT").open_buy_qty, 0.0);
    EXPECT_STREQ(to_string(order_status::expired), "expired");
}

TEST(OrderLedger, CancelWithoutFillReleasesEverything)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(3, "BTCUSDT", order_side::buy, 8.0));
    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_buy_qty, 8.0);

    ledger.set_status(3, order_status::cancelled);
    EXPECT_EQ(ledger.active_count(), 0u);
    EXPECT_DOUBLE_EQ(ledger.filled_qty(3), 0.0);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_buy_qty, 0.0);
}

TEST(OrderLedger, MultiplePartialFillsAccumulateExactly)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 9.0));

    const double steps[] = {2.0, 3.0, 1.5, 2.5};
    double filled = 0.0;
    for (double step : steps)
    {
        filled += step;
        ledger.on_fill(make_fill(1, "BTCUSDT", order_side::buy, step, 9.0 - filled));
        EXPECT_DOUBLE_EQ(ledger.filled_qty(1), filled);
        EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 9.0 - filled);
        EXPECT_NEAR(ledger.open_exposure("BTCUSDT").open_buy_qty, 9.0 - filled, 1e-12);
        EXPECT_EQ(ledger.active_count(), filled < 9.0 ? 1u : 0u);
    }
    EXPECT_EQ(ledger.get_order_status(1), order_status::filled);
}

TEST(OrderLedger, DuplicateFillIsNotBookedTwice)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 10.0));

    EXPECT_TRUE(ledger.on_fill(make_fill(1, "BTCUSDT", order_side::buy, 4.0, 6.0, /*fill_id=*/77)));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 4.0);

    // Same venue fill re-delivered (reconciler replay / transport retry).
    EXPECT_FALSE(ledger.on_fill(make_fill(1, "BTCUSDT", order_side::buy, 4.0, 6.0, /*fill_id=*/77)));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 4.0);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 6.0);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_buy_qty, 6.0);

    // A genuinely different fill id still applies.
    EXPECT_TRUE(ledger.on_fill(make_fill(1, "BTCUSDT", order_side::buy, 6.0, 0.0, /*fill_id=*/78)));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 10.0);
    EXPECT_EQ(ledger.get_order_status(1), order_status::filled);
}

TEST(OrderLedger, FillNeverExceedsOriginalQuantity)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 5.0));

    // Venue over-reports (or a stale duplicate with a fresh id arrives).
    ledger.on_fill(make_fill(1, "BTCUSDT", order_side::buy, 9.0, 0.0, /*fill_id=*/1));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 5.0);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 0.0);
    EXPECT_EQ(ledger.active_count(), 0u);
}

// A cancel that lands after the venue already filled the order must not
// resurrect capacity or exposure: the order is terminal either way.
TEST(OrderLedger, CancelAfterFillKeepsOrderTerminal)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 5.0));
    ledger.on_fill(make_fill(1, "BTCUSDT", order_side::buy, 5.0, 0.0));
    EXPECT_EQ(ledger.active_count(), 0u);

    ledger.set_status(1, order_status::cancelled);
    EXPECT_EQ(ledger.active_count(), 0u);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_buy_qty, 0.0);
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 5.0);
}

// The reverse race: a fill arriving for an order the engine already cancelled
// must be booked (the quantity is real) without re-opening the order.
TEST(OrderLedger, FillAfterCancelBooksQuantityWithoutReopening)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 5.0));
    ledger.set_status(1, order_status::cancelled);
    EXPECT_EQ(ledger.active_count(), 0u);

    ledger.on_fill(make_fill(1, "BTCUSDT", order_side::buy, 2.0, 3.0));
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 2.0);
    // A late fill is a documented residual risk, and the ledger records the
    // quantity, but the order's own state is driven by the fill: with 3.0
    // outstanding the venue considers it live again.
    EXPECT_EQ(ledger.get_order_status(1), order_status::partially_filled);
    EXPECT_EQ(ledger.active_count(), 1u);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_buy_qty, 3.0);
}

// ---------------------------------------------------------------------------
// Aggregation
// ---------------------------------------------------------------------------

TEST(OrderLedger, MultipleOrdersOnSameInstrumentAggregate)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 2.0));
    submit_and_ack(ledger, make_order(2, "BTCUSDT", order_side::buy, 3.0));
    submit_and_ack(ledger, make_order(3, "BTCUSDT", order_side::buy, 4.0));

    auto exp = ledger.open_exposure("BTCUSDT");
    EXPECT_DOUBLE_EQ(exp.open_buy_qty, 9.0);
    EXPECT_EQ(exp.open_order_count, 3u);
    EXPECT_EQ(ledger.active_count(), 3u);

    ledger.set_status(2, order_status::cancelled);
    exp = ledger.open_exposure("BTCUSDT");
    EXPECT_DOUBLE_EQ(exp.open_buy_qty, 6.0);
    EXPECT_EQ(exp.open_order_count, 2u);
    EXPECT_EQ(ledger.active_count(), 2u);
}

TEST(OrderLedger, SimultaneousBuyAndSellStayOnSeparateSides)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 2.0));
    submit_and_ack(ledger, make_order(2, "BTCUSDT", order_side::sell, 5.0));

    auto exp = ledger.open_exposure("BTCUSDT");
    EXPECT_DOUBLE_EQ(exp.open_buy_qty, 2.0);
    EXPECT_DOUBLE_EQ(exp.open_sell_qty, 5.0);
    EXPECT_EQ(exp.open_order_count, 2u);

    ledger.on_fill(make_fill(2, "BTCUSDT", order_side::sell, 1.0, 4.0));
    exp = ledger.open_exposure("BTCUSDT");
    EXPECT_DOUBLE_EQ(exp.open_buy_qty, 2.0);
    EXPECT_DOUBLE_EQ(exp.open_sell_qty, 4.0);
}

TEST(OrderLedger, MultipleInstrumentsDoNotBleedIntoEachOther)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 2.0));
    submit_and_ack(ledger, make_order(2, "ETHUSDT", order_side::buy, 7.0));
    submit_and_ack(ledger, make_order(3, "SOLUSDT", order_side::sell, 11.0));

    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_buy_qty, 2.0);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("ETHUSDT").open_buy_qty, 7.0);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("SOLUSDT").open_sell_qty, 11.0);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("SOLUSDT").open_buy_qty, 0.0);
    EXPECT_EQ(ledger.active_count(), 3u);

    ledger.set_status(2, order_status::filled);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("ETHUSDT").open_buy_qty, 0.0);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_buy_qty, 2.0);
    EXPECT_EQ(ledger.active_count(), 2u);
}

TEST(OrderLedger, UntrackedSymbolIsDistinguishableFromFlat)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 1.0));

    EXPECT_TRUE(ledger.tracks_symbol("BTCUSDT"));
    EXPECT_FALSE(ledger.tracks_symbol("DOGEUSDT"));
    EXPECT_DOUBLE_EQ(ledger.open_exposure("DOGEUSDT").open_buy_qty, 0.0);
    EXPECT_FALSE(ledger.symbol_capacity_exhausted());
}

// ---------------------------------------------------------------------------
// Amendments, reset, and counter independence
// ---------------------------------------------------------------------------

TEST(OrderLedger, AmendUpdatesOriginalQuantitySoTheSlotCanClose)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 10.0));
    ledger.amend(1, /*new_price=*/101.0, /*new_qty=*/4.0);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 4.0);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_buy_qty, 4.0);

    ledger.on_fill(make_fill(1, "BTCUSDT", order_side::buy, 4.0, 0.0));
    EXPECT_EQ(ledger.get_order_status(1), order_status::filled);
    EXPECT_EQ(ledger.active_count(), 0u);
}

TEST(OrderLedger, ReRegistrationKeepsFilledQuantity)
{
    OrderTracker ledger;
    const auto o = make_order(1, "BTCUSDT", order_side::buy, 10.0);
    submit_and_ack(ledger, o);
    ledger.on_fill(make_fill(1, "BTCUSDT", order_side::buy, 4.0, 6.0));

    // A pending stop converting to a market order re-enters with the same id.
    ledger.register_order(o);
    EXPECT_DOUBLE_EQ(ledger.filled_qty(1), 4.0);
    EXPECT_DOUBLE_EQ(ledger.pending_qty(1), 6.0);
    EXPECT_EQ(ledger.active_count(), 1u);
    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_buy_qty, 6.0);
}

TEST(OrderLedger, ResetClearsLedgerAndAggregates)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 2.0));
    submit_and_ack(ledger, make_order(2, "ETHUSDT", order_side::sell, 3.0));
    EXPECT_EQ(ledger.orders_seen(), 2u);

    ledger.reset();
    EXPECT_EQ(ledger.active_count(), 0u);
    EXPECT_EQ(ledger.orders_seen(), 0u);
    EXPECT_TRUE(ledger.get_open_orders().empty());
    EXPECT_DOUBLE_EQ(ledger.open_exposure("BTCUSDT").open_buy_qty, 0.0);
    EXPECT_FALSE(ledger.tracks_symbol("ETHUSDT"));
    EXPECT_EQ(ledger.get_order_status(1), order_status::unknown);
}

TEST(OrderLedger, OpenOrderSetMatchesNonTerminalEntriesExactly)
{
    OrderTracker ledger;
    submit_and_ack(ledger, make_order(1, "BTCUSDT", order_side::buy, 1.0));
    submit_and_ack(ledger, make_order(2, "BTCUSDT", order_side::buy, 1.0));
    submit_and_ack(ledger, make_order(3, "BTCUSDT", order_side::sell, 1.0));
    ledger.set_status(2, order_status::filled);
    ledger.set_status(3, order_status::expired);
    submit_and_ack(ledger, make_order(4, "ETHUSDT", order_side::buy, 1.0));

    const auto open = ledger.get_open_orders();
    EXPECT_EQ(open.size(), ledger.active_count());
    EXPECT_EQ(open.size(), 2u);
    for (auto id : open)
        EXPECT_TRUE(order_status_is_open(ledger.get_order_status(id)));

    std::size_t counted = 0;
    ledger.for_each_open([&](const order_ledger_entry& e) {
        ++counted;
        EXPECT_TRUE(e.is_open());
        EXPECT_GE(e.remaining_qty(), 0.0);
        EXPECT_LE(e.filled_qty, e.original_qty);
    });
    EXPECT_EQ(counted, 2u);
}
