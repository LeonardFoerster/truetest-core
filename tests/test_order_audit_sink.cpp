#include <gtest/gtest.h>
#include "engine/order_audit_sink.h"
#include "core/event.h"

#include <chrono>

static auto now() { return std::chrono::system_clock::now(); }

TEST(OrderAuditSink, NoopDoesNotCrash)
{
    NoopOrderAuditSink sink;

    auto ts = now();
    order_event o(ts, "AAPL", order_type::limit, order_side::buy, 10.0, 150.0);
    o.set_order_id(42);
    o.set_strategy_name("test_strat");

    sink.record_order_submitted(o, "pending");
    sink.record_status_transition(42, order_status::pending, order_status::open, "submitted");

    fill_event f(ts, "AAPL", 42, order_side::buy, 10.0, 150.0, 0.0);
    sink.record_fill(f, 42, "test_strat", "engine");

    sink.record_rejection(o, "test_cat", "test_detail");
    // Exercise rejection via the single (rich) shape. For identity-only cases the
    // caller synthesizes a minimal order_event (or the sink impl could resolve internally).
    order_event sparse_dummy(ts, "AAPL", order_type::limit, order_side::buy, 0.0);
    sparse_dummy.set_order_id(99);
    sink.record_rejection(sparse_dummy, "sparse_cat", "sparse_det");

    sink.record_cancellation(42, "AAPL", "test_strat", "user");
    sink.record_amendment(42, "AAPL", 150.0, 149.0, 10.0, 9.0, ts);

    funding_event fe(ts, "AAPL", 0.0, -0.5, "FUNDING_FEE");
    sink.record_funding(fe, "run_xxx");

    sink.record_event("test_event", "AAPL", "test_strat", 42, "info", "hello", "{\"k\":1}");

    EXPECT_EQ(sink.total_rejections(), 0u);

    auto h = sink.health();
    EXPECT_FALSE(h.connected);
    EXPECT_EQ(h.pending_lines, 0u);
    EXPECT_EQ(h.dropped_lines, 0u);
    EXPECT_EQ(h.fallback_lines, 0u);
}

#ifdef HAS_QUESTDB
TEST(OrderAuditSink, QuestdbOrderAuditSinkSkeleton)
{
    // Basic creation + calls for skeleton (null store is accepted; delegates safely).
    std::shared_ptr<truetest::questdb::QuestdbStore> store;
    bool active = false;
    QuestdbOrderAuditSink sink(store, &active);

    auto ts = now();
    order_event o(ts, "AAPL", order_type::limit, order_side::buy, 1.0, 100.0);
    o.set_order_id(1);
    o.set_strategy_name("s");

    sink.record_order_submitted(o, "pending");
    sink.record_status_transition(1, order_status::pending, order_status::open, nullptr);

    fill_event f(ts, "AAPL", 1, order_side::buy, 1.0, 100.0);
    sink.record_fill(f, 1, "s", "src");

    sink.record_rejection(o, "cat", "det");
    order_event sparse_dummy(ts, "SYM", order_type::limit, order_side::buy, 0.0);
    sparse_dummy.set_order_id(2);
    sink.record_rejection(sparse_dummy, "scat", "sdet");

    sink.record_cancellation(1, "AAPL", "s", "r");
    sink.record_amendment(1, "AAPL", 100.0, 99.0, 1.0, 0.5, ts);

    funding_event fe(ts, "AAPL", 0.0, 0.0);
    sink.record_funding(fe, "run");

    sink.record_event("e", "AAPL", "s", 1, "low", "m", "");

    // increments happened for the two rejections (even on inactive skeleton)
    EXPECT_EQ(sink.total_rejections(), 2u);

    auto h = sink.health();
    EXPECT_FALSE(h.connected);
}
#endif
