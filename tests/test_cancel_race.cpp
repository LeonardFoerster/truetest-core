#include <gtest/gtest.h>
#include "execution/execution_adapter.h"
#include "execution/latency_model.h"

#include <chrono>
#include <memory>

using namespace std::chrono_literals;

namespace {

auto t0() { return std::chrono::system_clock::time_point(std::chrono::milliseconds(0)); }

}

// -----------------------------------------------------------------------
// LocalBookAdapter with cancel latency - the central scenario: a paper
// maker posts a limit, decides to cancel, and the exchange book crosses
// during the in-flight cancel window.
// -----------------------------------------------------------------------

TEST(LocalBookAdapterCancelRace, CancelInflight_TradeCrosses_Fills)
{
    auto ob = std::make_shared<orderbook>();
    auto lat = std::make_shared<FixedLatencyModel>(
        /*order*/ latency_duration(0),
        /*md*/    latency_duration(0),
        /*cancel*/ std::chrono::duration_cast<latency_duration>(100ms));
    LocalBookAdapter adapter(ob, nullptr, nullptr,
                             /*rng*/42, /*agg*/1.1, /*qty_scale*/1e8, lat);

    // Seed our buy-limit at 100.
    order_event buy(t0(), "X", order_type::limit, order_side::buy, 10, 100.0);
    buy.set_order_id(42);
    buy.set_earliest_eligible_ts(t0());
    adapter.submit_order(buy);
    adapter.advance_time(t0());

    // T=50ms: cancel requested. Eligibility = T + 100ms = 150ms.
    adapter.advance_time(t0() + 50ms);
    EXPECT_TRUE(adapter.cancel_order(42));

    // T=80ms: a counterparty sell crosses our 100 price. Cancel still
    // in-flight - the order is still on the book and must fill.
    adapter.advance_time(t0() + 80ms);
    order_event counter(t0() + 80ms, "X", order_type::limit,
                        order_side::sell, 10, 100.0);
    counter.set_order_id(99);
    counter.set_earliest_eligible_ts(t0() + 80ms);
    adapter.submit_order(counter);

    // LocalBookAdapter only emits fill_events for the submitting order.
    // The proof that our resting order 42 was still on the book is that
    // the counter-sell (order 99) matched and got a fill - if our in-flight
    // cancel had been applied immediately, there'd be no liquidity at 100
    // and the counter-sell would rest instead of filling.
    std::vector<fill_event> fills;
    ASSERT_TRUE(adapter.poll_fills(fills));
    bool counter_filled = false;
    for (const auto& f : fills)
    {
        if (f.get_order_id() == 99) { counter_filled = true; break; }
    }
    EXPECT_TRUE(counter_filled)
        << "Counter-sell didn't match - resting buy was removed before the "
           "cancel window elapsed, which is the bug this test guards against.";
}

TEST(LocalBookAdapterCancelRace, CancelAfterWindow_TradeDoesNotFill)
{
    auto ob = std::make_shared<orderbook>();
    auto lat = std::make_shared<FixedLatencyModel>(
        latency_duration(0),
        latency_duration(0),
        std::chrono::duration_cast<latency_duration>(100ms));
    LocalBookAdapter adapter(ob, nullptr, nullptr, 42, 1.1, 1e8, lat);

    order_event buy(t0(), "X", order_type::limit, order_side::buy, 10, 100.0);
    buy.set_order_id(42);
    buy.set_earliest_eligible_ts(t0());
    adapter.submit_order(buy);
    adapter.advance_time(t0());

    // T=50ms: cancel requested (eligible at 150ms).
    adapter.advance_time(t0() + 50ms);
    adapter.cancel_order(42);

    // T=200ms: advance past the eligibility -> cancel drained; order gone.
    adapter.advance_time(t0() + 200ms);

    // Counterparty sell at our price arrives AFTER cancel completed.
    order_event counter(t0() + 250ms, "X", order_type::limit,
                        order_side::sell, 10, 100.0);
    counter.set_order_id(99);
    counter.set_earliest_eligible_ts(t0() + 250ms);
    adapter.submit_order(counter);
    adapter.advance_time(t0() + 250ms);

    std::vector<fill_event> fills;
    adapter.poll_fills(fills);
    for (const auto& f : fills)
    {
        EXPECT_NE(f.get_order_id(), 42u)
            << "Cancel had elapsed - our order must NOT have filled.";
    }
}

TEST(LocalBookAdapterCancelRace, ZeroLatency_CancelIsImmediate)
{
    // ZeroLatencyModel -> cancel_latency = 0 -> current behavior.
    auto ob = std::make_shared<orderbook>();
    auto lat = std::make_shared<ZeroLatencyModel>();
    LocalBookAdapter adapter(ob, nullptr, nullptr, 42, 1.1, 1e8, lat);

    order_event buy(t0(), "X", order_type::limit, order_side::buy, 10, 100.0);
    buy.set_order_id(42);
    buy.set_earliest_eligible_ts(t0());
    adapter.submit_order(buy);
    adapter.advance_time(t0());

    adapter.cancel_order(42);
    adapter.advance_time(t0());  // same instant -> eligible immediately.

    // Now a counterparty cross arrives. Order should be gone.
    order_event counter(t0() + 1ms, "X", order_type::limit,
                        order_side::sell, 10, 100.0);
    counter.set_order_id(99);
    counter.set_earliest_eligible_ts(t0() + 1ms);
    adapter.submit_order(counter);
    adapter.advance_time(t0() + 1ms);

    std::vector<fill_event> fills;
    adapter.poll_fills(fills);
    for (const auto& f : fills)
        EXPECT_NE(f.get_order_id(), 42u);
}

TEST(LocalBookAdapterCancelRace, NoLatencyModel_CancelIsImmediate_BackCompat)
{
    // Without any latency model attached, existing behavior must be
    // preserved - cancel removes the order instantly.
    auto ob = std::make_shared<orderbook>();
    LocalBookAdapter adapter(ob, nullptr, nullptr);   // no latency model

    order_event buy(t0(), "X", order_type::limit, order_side::buy, 10, 100.0);
    buy.set_order_id(42);
    buy.set_earliest_eligible_ts(t0());
    adapter.submit_order(buy);

    EXPECT_TRUE(adapter.cancel_order(42));

    order_event counter(t0() + 1ms, "X", order_type::limit,
                        order_side::sell, 10, 100.0);
    counter.set_order_id(99);
    counter.set_earliest_eligible_ts(t0() + 1ms);
    adapter.submit_order(counter);

    std::vector<fill_event> fills;
    adapter.poll_fills(fills);
    for (const auto& f : fills)
        EXPECT_NE(f.get_order_id(), 42u);
}
