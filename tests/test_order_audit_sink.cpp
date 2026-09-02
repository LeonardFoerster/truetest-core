#include <gtest/gtest.h>
#include "engine/order_audit_sink.h"
#include "core/event.h"

#include <chrono>
#include <string>

static auto now() { return std::chrono::system_clock::now(); }

namespace {
class RecordingAuditSink final : public IOrderAuditSink {
public:
    void record_order_submitted(const order_event&, const char*) override {}
    void record_status_transition(uint64_t, order_status, order_status, const char*) override {}
    void record_fill(const fill_event&, uint64_t, const char*, const char*) override {}
    void record_rejection(const order_event&, const char*, const char*) override {}
    void record_cancellation(uint64_t, const char*, const char*, const char*) override {}
    void record_amendment(uint64_t, const char*, double, double, double, double,
                          std::chrono::system_clock::time_point) override {}
    void record_funding(const funding_event&, const char*) override {}
    void record_event(const char* event_type, const char*, const char*, uint64_t,
                      const char* severity, const char*, const char* details) override
    {
        type = event_type ? event_type : "";
        phase = severity ? severity : "";
        json = details ? details : "";
    }
    void record_exit_lifecycle(const exit_lifecycle_record& record) override
    {
        ++exit_lifecycle_calls;
        signal_id = record.signal_id;
        order_id = record.order_id;
        opener_order_id = record.opener_order_id;
        fill_id = record.fill_id;
        requested_qty = record.requested_qty;
        filled_qty = record.filled_qty;
        remaining_qty = record.remaining_qty;
        risk_outcome = record.risk_outcome ? record.risk_outcome : "";
        phase = record.phase ? record.phase : "";
    }

    std::string type;
    std::string phase;
    std::string json;
    std::string risk_outcome;
    std::size_t exit_lifecycle_calls = 0;
    std::uint64_t signal_id = 0;
    std::uint64_t order_id = 0;
    std::uint64_t opener_order_id = 0;
    std::uint64_t fill_id = 0;
    double requested_qty = 0.0;
    double filled_qty = 0.0;
    double remaining_qty = 0.0;
};
}

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

    // Noop still records rejection counts (for soft-risk tests without QuestDB);
    // other record_* remain no-ops.
    EXPECT_EQ(sink.total_rejections(), 2u);

    auto h = sink.health();
    EXPECT_FALSE(h.connected);
    EXPECT_EQ(h.pending_lines, 0u);
    EXPECT_EQ(h.dropped_lines, 0u);
    EXPECT_EQ(h.fallback_lines, 0u);
}

TEST(OrderAuditSink, ExitLifecycleRecordExportsTheRequiredJoinFields)
{
    RecordingAuditSink sink;
    const auto ts = now();
    sink.record_exit_lifecycle(exit_lifecycle_record{
        /*signal_id=*/11, /*order_id=*/12, /*opener_order_id=*/10, /*fill_id=*/99,
        ts, ts + std::chrono::milliseconds(1), ts + std::chrono::milliseconds(2),
        ts + std::chrono::milliseconds(3),
        /*requested=*/5.0, /*filled=*/2.0, /*remaining=*/3.0,
        order_exit_reason::stop_loss, order_status::partially_filled,
        order_status::rejected, "BTCUSDT", "sma", "venue_filter", "terminal"});

    EXPECT_EQ(sink.exit_lifecycle_calls, 1U);
    EXPECT_EQ(sink.phase, "terminal");
    EXPECT_EQ(sink.signal_id, 11U);
    EXPECT_EQ(sink.order_id, 12U);
    EXPECT_EQ(sink.opener_order_id, 10U);
    EXPECT_EQ(sink.fill_id, 99U);
    EXPECT_DOUBLE_EQ(sink.requested_qty, 5.0);
    EXPECT_DOUBLE_EQ(sink.filled_qty, 2.0);
    EXPECT_DOUBLE_EQ(sink.remaining_qty, 3.0);
    EXPECT_EQ(sink.risk_outcome, "venue_filter");
    EXPECT_TRUE(sink.type.empty());
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
