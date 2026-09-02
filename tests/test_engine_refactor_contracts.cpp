#include <gtest/gtest.h>

#include "core/event.h"
#include "engine/engine.h"
#include "engine/engine_hotpath_sink.h"
#include "engine/pending_order_scheduler.h"
#include "engine/risk_unwind_sink.h"

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

static_assert(std::is_final_v<engine>);
static_assert(std::is_base_of_v<IEngineHotPathSink, engine>);
static_assert(std::is_base_of_v<IRiskUnwindSink, engine>);
static_assert(!std::is_convertible_v<engine*, IEngineHotPathSink*>);
static_assert(!std::is_convertible_v<engine*, IRiskUnwindSink*>);
static_assert(requires(engine& value) { value.trigger_halt(std::string_view{}); });

namespace {

std::shared_ptr<order_event> make_order(std::string symbol, std::uint64_t id)
{
    auto order =
        std::make_shared<order_event>(std::chrono::system_clock::time_point{}, std::move(symbol),
                                      order_type::limit, order_side::buy, 1.0, 100.0);
    order->set_order_id(id);
    return order;
}

}  // namespace

TEST(PendingOrderSchedulerContracts, EmptyLatencyPopReturnsNull)
{
    PendingOrderScheduler scheduler;

    EXPECT_EQ(scheduler.pop_due_latency(), nullptr);
}

TEST(PendingOrderSchedulerContracts, LatencyPopPreservesDueOrder)
{
    PendingOrderScheduler scheduler;
    const auto first_due = std::chrono::system_clock::time_point{std::chrono::milliseconds{1}};
    const auto second_due = std::chrono::system_clock::time_point{std::chrono::milliseconds{2}};
    auto later = make_order("BTCUSDT", 2);
    auto earlier = make_order("BTCUSDT", 1);
    later->set_earliest_eligible_ts(second_due);
    earlier->set_earliest_eligible_ts(first_due);

    scheduler.schedule_latency(later, scheduler.next_seq());
    scheduler.schedule_latency(earlier, scheduler.next_seq());

    EXPECT_FALSE(scheduler.latency_due(first_due - std::chrono::milliseconds{1}));
    ASSERT_TRUE(scheduler.latency_due(first_due));
    EXPECT_EQ(scheduler.pop_due_latency(), earlier);
    EXPECT_FALSE(scheduler.latency_due(first_due));
    ASSERT_TRUE(scheduler.latency_due(second_due));
    EXPECT_EQ(scheduler.pop_due_latency(), later);
    EXPECT_EQ(scheduler.pop_due_latency(), nullptr);
}

TEST(PendingOrderSchedulerContracts, InvalidReadyAccessPreservesEvidence)
{
    PendingOrderScheduler scheduler;
    scheduler.reserve_bar_delay_capacity(2);
    auto first = make_order("BTCUSDT", 1);
    auto second = make_order("BTCUSDT", 2);

    scheduler.schedule_bar_delay(first, scheduler.next_seq(), 1);
    scheduler.schedule_bar_delay(second, scheduler.next_seq(), 1);
    ASSERT_TRUE(scheduler.compact_bar_delay_due("BTCUSDT"));
    ASSERT_EQ(scheduler.ready_count(), 2U);

    EXPECT_EQ(scheduler.take_ready_order(2), nullptr);
    scheduler.retain_ready_suffix(3);
    EXPECT_EQ(scheduler.ready_count(), 2U);
    EXPECT_EQ(scheduler.take_ready_order(0), first);
    EXPECT_EQ(scheduler.take_ready_order(1), second);
}

TEST(PendingOrderSchedulerContracts, RetainedSuffixMergesInSequenceOrder)
{
    PendingOrderScheduler scheduler;
    scheduler.reserve_bar_delay_capacity(2);
    auto due_first = make_order("ETHUSDT", 10);
    auto due_later = make_order("ETHUSDT", 11);

    scheduler.schedule_bar_delay(due_first, scheduler.next_seq(), 1);
    scheduler.schedule_bar_delay(due_later, scheduler.next_seq(), 2);
    ASSERT_TRUE(scheduler.compact_bar_delay_due("ETHUSDT"));
    ASSERT_EQ(scheduler.ready_count(), 1U);

    scheduler.retain_ready_suffix(0);
    EXPECT_EQ(scheduler.ready_count(), 0U);
    ASSERT_TRUE(scheduler.compact_bar_delay_due("ETHUSDT"));
    ASSERT_EQ(scheduler.ready_count(), 2U);
    EXPECT_EQ(scheduler.take_ready_order(0), due_first);
    EXPECT_EQ(scheduler.take_ready_order(1), due_later);
}
