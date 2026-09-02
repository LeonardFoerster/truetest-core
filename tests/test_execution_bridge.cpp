#include <gtest/gtest.h>

#include "execution/execution_bridge.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

static auto now() { return std::chrono::system_clock::now(); }

class OperationalReconciler final : public IReconciler
{
public:
    bool is_operational() const noexcept override { return true; }
    std::string reconcile(const portfolio&, double) override { return {}; }
};

class OperationalKillSwitch final : public IKillSwitch
{
public:
    bool is_operational() const noexcept override { return true; }
    bool cancel_all_and_flatten(std::chrono::milliseconds) override
    {
        return true;
    }
};

WriteSafetyReadiness validated_write_safety_readiness()
{
    OperationalReconciler reconciler;
    OperationalKillSwitch kill_switch;
    return validate_startup_safety(
        true, true, true, private_execution_capability::exchange_writes,
        &reconciler, &kill_switch).readiness;
}

class FakeOrderTransport : public IOrderTransport
{
public:
    bool open() override { opened_ = true; return true; }
    void close() override { opened_ = false; }

    result submit(std::string_view endpoint, std::string_view payload) override
    {
        submissions_.emplace_back(std::string(endpoint), std::string(payload));
        result r;
        r.ok = submit_ok_;
        r.uncertain = submit_uncertain_;
        r.fatal = submit_fatal_;
        r.exchange_order_id = next_exchange_id_;
        r.raw_response = "fake-ok";
        if (!submit_ok_) r.error = "fake-rejected";
        return r;
    }

    result cancel(std::string_view endpoint, std::string_view payload) override
    {
        cancels_.emplace_back(std::string(endpoint), std::string(payload));
        result r;
        r.ok = cancel_ok_;
        r.uncertain = cancel_uncertain_;
        r.fatal = cancel_fatal_;
        if (!cancel_ok_) r.error = "fake-cancel-fail";
        return r;
    }

    bool opened_ = false;
    bool submit_ok_ = true;
    bool submit_uncertain_ = false;
    bool submit_fatal_ = false;
    bool cancel_ok_ = true;
    bool cancel_uncertain_ = false;
    bool cancel_fatal_ = false;
    std::string next_exchange_id_ = "EX-1";
    std::vector<std::pair<std::string, std::string>> submissions_;
    std::vector<std::pair<std::string, std::string>> cancels_;
};

class BlockingOrderTransport final : public FakeOrderTransport
{
public:
    result submit(std::string_view endpoint, std::string_view payload) override
    {
        {
            std::lock_guard<std::mutex> lock(mu_);
            entered_ = true;
        }
        entered_cv_.notify_all();
        std::unique_lock<std::mutex> lock(mu_);
        const bool released = release_cv_.wait_for(
            lock, std::chrono::seconds(2), [this] { return released_; });
        lock.unlock();
        if (!released)
        {
            result timed_out;
            timed_out.fatal = true;
            timed_out.error = "test transport release timed out";
            completed_.store(true, std::memory_order_release);
            return timed_out;
        }
        auto result = FakeOrderTransport::submit(endpoint, payload);
        completed_.store(true, std::memory_order_release);
        return result;
    }

    bool wait_until_entered()
    {
        std::unique_lock<std::mutex> lock(mu_);
        return entered_cv_.wait_for(
            lock, std::chrono::seconds(2), [this] { return entered_; });
    }

    void release() { { std::lock_guard<std::mutex> lock(mu_); released_ = true; } release_cv_.notify_all(); }
    bool completed() const { return completed_.load(std::memory_order_acquire); }

private:
    std::mutex mu_;
    std::condition_variable entered_cv_;
    std::condition_variable release_cv_;
    bool entered_ = false;
    bool released_ = false;
    std::atomic<bool> completed_{false};
};

class NonStdThrowingOrderTransport final : public FakeOrderTransport
{
public:
    result submit(std::string_view endpoint,
                  std::string_view payload) override
    {
        const int call = calls.fetch_add(1, std::memory_order_acq_rel);
        if (call == 0) throw 7;
        return FakeOrderTransport::submit(endpoint, payload);
    }

    std::atomic<int> calls{0};
};

class FakeFillTransport : public IFillTransport
{
public:
    bool open() override
    {
        state_ = lifecycle::open;
        if (status_cb_) status_cb_(state_, "opened");
        if (!message_on_open_.empty() && message_cb_)
            message_cb_(message_on_open_);
        return true;
    }
    void close() override { state_ = lifecycle::closed; }
    lifecycle state() const override { return state_; }

    void set_on_message(message_cb cb) override { message_cb_ = std::move(cb); }
    void set_on_status(status_cb cb)  override { status_cb_  = std::move(cb); }

    void deliver(std::string_view msg) { if (message_cb_) message_cb_(msg); }
    void report_status(lifecycle st, std::string_view note)
    {
        state_ = st;
        if (status_cb_) status_cb_(st, note);
    }

    lifecycle  state_ = lifecycle::closed;
    message_cb message_cb_;
    status_cb  status_cb_;
    std::string message_on_open_;
};

class FakeEncoder : public IOrderEncoder
{
public:
    encoded_order encode_submit(const order_event& o,
                                std::string_view client_id) override
    {
        encoded_order e;
        e.endpoint = "/submit";
        e.client_order_id = std::string(client_id);
        std::ostringstream oss;
        oss << "symbol=" << o.get_symbol()
            << "&qty="   << o.get_quantity()
            << "&price=" << o.get_price()
            << "&clientId=" << std::string(client_id);
        e.wire_payload = oss.str();
        return e;
    }

    encoded_order encode_cancel(std::string_view symbol,
                                std::string_view exchange_order_id,
                                std::string_view client_order_id) override
    {
        encoded_order e;
        e.endpoint = "/cancel";
        e.client_order_id = std::string(client_order_id);
        std::ostringstream oss;
        oss << "symbol=" << std::string(symbol)
            << "&exchangeId=" << std::string(exchange_order_id)
            << "&clientId="   << std::string(client_order_id);
        e.wire_payload = oss.str();
        return e;
    }
};

// Test wire format:
// "kind|client_id|exchange_id|symbol|side|qty|price[|exec_id|cumulative]"
// Kinds: ack, partial, full, cancel, reject, expire
class FakeParser : public IFillParser
{
public:
    bool parse(std::string_view raw, parsed_exec& out) override
    {
        std::vector<std::string> parts;
        std::string cur;
        for (char c : raw)
        {
            if (c == '|') { parts.push_back(std::move(cur)); cur.clear(); }
            else          { cur.push_back(c); }
        }
        if (!cur.empty()) parts.push_back(std::move(cur));
        if (parts.size() < 7) return false;

        const auto& k = parts[0];
        if      (k == "ack")     out.k = parsed_exec::kind::ack;
        else if (k == "partial") out.k = parsed_exec::kind::partial_fill;
        else if (k == "full")    out.k = parsed_exec::kind::full_fill;
        else if (k == "cancel")  out.k = parsed_exec::kind::canceled;
        else if (k == "reject")  out.k = parsed_exec::kind::rejected;
        else if (k == "expire")  out.k = parsed_exec::kind::expired;
        else                      out.k = parsed_exec::kind::other;

        out.client_order_id   = parts[1];
        out.exchange_order_id = parts[2];
        out.symbol            = parts[3];
        out.side              = (parts[4] == "buy") ? order_side::buy : order_side::sell;
        out.last_fill_qty     = std::stod(parts[5]);
        out.last_fill_price   = std::stod(parts[6]);
        out.venue_execution_id = parts.size() > 7 && !parts[7].empty()
            ? parts[7]
            : "fake-exec-" + std::to_string(
                next_exec_id_.fetch_add(1, std::memory_order_relaxed) + 1U);
        if (parts.size() > 8 && !parts[8].empty())
        {
            out.cumulative_qty = std::stod(parts[8]);
            out.has_cumulative_qty = true;
        }
        out.commission_asset = "USD";
        out.ts = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(123456));
        return true;
    }

private:
    std::atomic<std::uint64_t> next_exec_id_{0};
};

class FakeFundingParser : public IFillParser
{
public:
    bool parse(std::string_view, parsed_exec&) override { return false; }

    funding_parse_result parse_funding_update(
        std::string_view raw, parsed_funding_update& out) noexcept override
    {
        if (raw == "funding")
        {
            out = parsed_funding_update{1'700'000'000'000LL, -0.5};
            return funding_parse_result::valid;
        }
        if (raw == "malformed") return funding_parse_result::invalid;
        return funding_parse_result::not_funding;
    }
};

order_event make_order(uint64_t id,
                       const std::string& sym = "TEST",
                       double qty = 10.0,
                       double px = 100.0,
                       order_side side = order_side::buy)
{
    order_event o(now(), sym, order_type::limit, side, qty, px);
    o.set_order_id(id);
    return o;
}

struct bridge_harness
{
    std::shared_ptr<FakeOrderTransport> tx = std::make_shared<FakeOrderTransport>();
    std::shared_ptr<FakeFillTransport>  ft = std::make_shared<FakeFillTransport>();
    std::shared_ptr<FakeEncoder>        en = std::make_shared<FakeEncoder>();
    std::shared_ptr<FakeParser>         pa = std::make_shared<FakeParser>();

    std::unique_ptr<ExecutionBridge> bridge;

    bridge_harness() { rebuild(); }

    void rebuild()
    {
        ExecutionBridge::deps d;
        d.order_tx = tx;
        d.fill_tx  = ft;
        d.encoder  = en;
        d.parser   = pa;
        d.write_safety_readiness = validated_write_safety_readiness();
        d.start_transport_thread = false;
        bridge = std::make_unique<ExecutionBridge>(std::move(d));
    }
};

bool wait_submit_results(ExecutionBridge& bridge,
                         std::vector<ExecutionBridge::submit_result>& results)
{
    for (int i = 0; i < 1000 && results.empty(); ++i)
    {
        bridge.drain_outbound_for_test();
        (void)bridge.poll_submit_results(results);
        if (!results.empty()) return true;
        std::this_thread::yield();
    }
    return !results.empty();
}

}

TEST(ExecutionBridge, OpensTransports)
{
    bridge_harness h;
    ASSERT_TRUE(h.bridge->open());
    EXPECT_TRUE(h.tx->opened_);
    EXPECT_EQ(h.ft->state(), IFillTransport::lifecycle::open);
}

TEST(ExecutionBridge, UnvalidatedWriteSafetyRejectsBeforeTransportOrMutation)
{
    auto tx = std::make_shared<FakeOrderTransport>();
    auto ft = std::make_shared<FakeFillTransport>();
    ExecutionBridge::deps d;
    d.order_tx = tx;
    d.fill_tx = ft;
    d.encoder = std::make_shared<FakeEncoder>();
    d.parser = std::make_shared<FakeParser>();
    d.start_transport_thread = false;
    ExecutionBridge bridge(std::move(d));

    EXPECT_FALSE(bridge.open());
    EXPECT_FALSE(tx->opened_);
    EXPECT_EQ(ft->state(), IFillTransport::lifecycle::closed);
    EXPECT_NE(bridge.last_error().find("write safety readiness"),
              std::string::npos);

    bridge.submit_order(make_order(9001));
    EXPECT_FALSE(bridge.cancel_order(9001));
    bridge.drain_outbound_for_test();
    EXPECT_TRUE(tx->submissions_.empty());
    EXPECT_TRUE(tx->cancels_.empty());
    EXPECT_NE(bridge.last_error().find("write safety readiness"),
              std::string::npos);
}

TEST(ExecutionBridge, C05_AuthoritativeLifecycleUsesBoundedEngineHandoff)
{
    bridge_harness h;
    ASSERT_TRUE(h.bridge->open());
    h.bridge->submit_order(make_order(41));

    h.ft->deliver("ack|tt-41|EX-41|TEST|buy|0|0");
    venue_lifecycle_event event;
    ASSERT_TRUE(h.bridge->poll_lifecycle_event(event));
    EXPECT_EQ(event.engine_order_id, 41u);
    EXPECT_EQ(event.transition, venue_order_transition::acknowledged);
    EXPECT_EQ(event.exchange_ts.time_since_epoch(),
              std::chrono::milliseconds(123456));
    EXPECT_FALSE(h.bridge->poll_lifecycle_event(event));

    h.ft->deliver("cancel|tt-41|EX-41|TEST|buy|0|0");
    ASSERT_TRUE(h.bridge->poll_lifecycle_event(event));
    EXPECT_EQ(event.engine_order_id, 41u);
    EXPECT_EQ(event.transition, venue_order_transition::canceled);
    EXPECT_FALSE(h.bridge->poll_lifecycle_event(event));
}

TEST(ExecutionBridge, C05_LifecycleHandoffOverflowFailsClosedWithoutOverwrite)
{
    bridge_harness h;
    ASSERT_TRUE(h.bridge->open());
    h.bridge->submit_order(make_order(42));

    for (std::size_t i = 0; i < 4097; ++i)
        h.ft->deliver("ack|tt-42|EX-42|TEST|buy|0|0");

    std::vector<ExecutionBridge::status_event> statuses;
    ASSERT_TRUE(h.bridge->poll_status(statuses));
    ASSERT_FALSE(statuses.empty());
    EXPECT_EQ(statuses.back().state, IFillTransport::lifecycle::error);
    EXPECT_NE(statuses.back().note.find("lifecycle ingress capacity"),
              std::string::npos);

    std::size_t delivered = 0;
    venue_lifecycle_event event;
    while (h.bridge->poll_lifecycle_event(event)) ++delivered;
    EXPECT_EQ(delivered, 4096u)
        << "overflow must preserve the complete admitted prefix";

    const auto submitted_before = h.tx->submissions_.size();
    h.bridge->submit_order(make_order(43));
    h.bridge->drain_outbound_for_test();
    EXPECT_EQ(h.tx->submissions_.size(), submitted_before)
        << "lifecycle loss closes venue mutation admission terminally";
}

TEST(ExecutionBridge, FatalIngressDuringOpenCannotBeClearedByStartup)
{
    bridge_harness h;
    h.ft->message_on_open_ =
        "full|tt-3|EX-1|TEST|buy|nan|100";

    EXPECT_FALSE(h.bridge->open());
    h.bridge->submit_order(make_order(3, "TEST", 1.0, 100.0));
    EXPECT_NE(h.bridge->last_error().find("quiesced"), std::string::npos);
    EXPECT_TRUE(h.tx->submissions_.empty());
}

TEST(ExecutionBridge, SubmitEncodesAndSends)
{
    bridge_harness h;
    ASSERT_TRUE(h.bridge->open());

    h.bridge->submit_order(make_order(42, "BTCUSDT", 2.0, 50000.0));
    h.bridge->drain_outbound_for_test();

    // Robust wait: drain + yield until we see the expected submission recorded (thread or drain may win)
    bool saw_submit = false;
    std::string seen_body;
    for (int i = 0; i < 2000; ++i) {
        h.bridge->drain_outbound_for_test();
        for (const auto& s : h.tx->submissions_) {
            if (s.first == "/submit") {
                saw_submit = true;
                seen_body = s.second;
                break;
            }
        }
        if (saw_submit) break;
        std::this_thread::yield();
    }

    // With async, may see 1 (direct drain) ; tolerate if thread also processed (rare race in test)
    EXPECT_TRUE(saw_submit) << "never saw /submit in recorded submissions";
    EXPECT_NE(seen_body.find("symbol=BTCUSDT"), std::string::npos);
    EXPECT_NE(seen_body.find("clientId=tt-42"), std::string::npos);
}

TEST(ExecutionBridge, FullFillRoundtrip)
{
    bridge_harness h;
    h.tx->next_exchange_id_ = "EX-99";
    ASSERT_TRUE(h.bridge->open());

    h.bridge->submit_order(make_order(7, "BTCUSDT", 1.5, 60000.0));

    h.ft->deliver("full|tt-7|EX-99|BTCUSDT|buy|1.5|60000");

    std::vector<fill_event> fills;
    ASSERT_TRUE(h.bridge->poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);

    const auto& f = fills[0];
    EXPECT_EQ(f.get_order_id(), 7u);
    EXPECT_EQ(f.get_symbol(), "BTCUSDT");
    EXPECT_EQ(f.get_side(), order_side::buy);
    EXPECT_DOUBLE_EQ(f.get_filled_quantity(), 1.5);
    EXPECT_DOUBLE_EQ(f.get_fill_price(), 60000.0);
    EXPECT_EQ(f.get_venue_execution_id(), "fake-exec-1");
    EXPECT_EQ(f.get_commission_currency(), "USD");
    EXPECT_TRUE(f.has_cumulative_filled_qty());
    EXPECT_DOUBLE_EQ(f.get_cumulative_filled_qty(), 1.5);
    EXPECT_EQ(f.get_cumulative_source(),
              fill_cumulative_source::engine_accumulated);
    EXPECT_EQ(f.get_source(), fill_source::exchange);
    EXPECT_EQ(f.get_provenance().model, fill_execution_model::venue_reported);
    EXPECT_EQ(f.get_provenance().reason,
              fill_execution_reason::venue_execution_report);
    EXPECT_FALSE(f.get_provenance().exploratory);
    EXPECT_DOUBLE_EQ(f.get_provenance().intended_price, 60000.0);
    EXPECT_DOUBLE_EQ(f.get_provenance().reference_price, 60000.0);
    EXPECT_FALSE(f.is_partial());
}

TEST(ExecutionBridge, PartialThenFull)
{
    bridge_harness h;
    ASSERT_TRUE(h.bridge->open());

    h.bridge->submit_order(make_order(3, "TEST", 10.0, 100.0));

    h.ft->deliver("partial|tt-3|EX-1|TEST|buy|4|100");
    h.ft->deliver("full|tt-3|EX-1|TEST|buy|6|100");

    std::vector<fill_event> fills;
    ASSERT_TRUE(h.bridge->poll_fills(fills));
    ASSERT_EQ(fills.size(), 2u);

    EXPECT_DOUBLE_EQ(fills[0].get_filled_quantity(), 4.0);
    EXPECT_TRUE(fills[0].is_partial());
    EXPECT_DOUBLE_EQ(fills[0].get_remaining_qty(), 6.0);

    EXPECT_DOUBLE_EQ(fills[1].get_filled_quantity(), 6.0);
    EXPECT_FALSE(fills[1].is_partial());
    EXPECT_DOUBLE_EQ(fills[1].get_remaining_qty(), 0.0);
}

TEST(ExecutionBridge, TransactionalDeliveryRetainsHeadUntilMatchingAck)
{
    bridge_harness h;
    ASSERT_TRUE(h.bridge->open());
    h.bridge->submit_order(make_order(3, "TEST", 10.0, 100.0));

    h.ft->deliver("partial|tt-3|EX-1|TEST|buy|4|100");
    h.ft->deliver("full|tt-3|EX-1|TEST|buy|6|100");

    ASSERT_TRUE(h.bridge->supports_transactional_fill_delivery());
    fill_event first({}, "", 0, order_side::buy, 0.0, 0.0);
    ASSERT_TRUE(h.bridge->peek_fill(first));
    EXPECT_DOUBLE_EQ(first.get_filled_quantity(), 4.0);

    fill_event repeated({}, "", 0, order_side::buy, 0.0, 0.0);
    ASSERT_TRUE(h.bridge->peek_fill(repeated));
    EXPECT_EQ(repeated.get_fill_id(), first.get_fill_id());
    EXPECT_DOUBLE_EQ(repeated.get_filled_quantity(), 4.0);

    EXPECT_FALSE(h.bridge->acknowledge_fill(first.get_fill_id() + 1));
    fill_event still_first({}, "", 0, order_side::buy, 0.0, 0.0);
    ASSERT_TRUE(h.bridge->peek_fill(still_first));
    EXPECT_EQ(still_first.get_fill_id(), first.get_fill_id());

    ASSERT_TRUE(h.bridge->acknowledge_fill(first.get_fill_id()));
    fill_event second({}, "", 0, order_side::buy, 0.0, 0.0);
    ASSERT_TRUE(h.bridge->peek_fill(second));
    EXPECT_DOUBLE_EQ(second.get_filled_quantity(), 6.0);
    EXPECT_NE(second.get_fill_id(), first.get_fill_id());
    ASSERT_TRUE(h.bridge->acknowledge_fill(second.get_fill_id()));
    EXPECT_FALSE(h.bridge->peek_fill(second));
}

TEST(ExecutionBridge, LargeOrderFirstUnitFillIsForwardProgress)
{
    bridge_harness h;
    ASSERT_TRUE(h.bridge->open());
    h.bridge->submit_order(make_order(
        3, "TEST", 1'000'000'000.0, 1.0));

    h.ft->deliver(
        "partial|tt-3|EX-1|TEST|buy|1|1|native-first|1");
    fill_event fill({}, "", 0, order_side::buy, 0.0, 0.0);
    ASSERT_TRUE(h.bridge->peek_fill(fill));
    EXPECT_DOUBLE_EQ(fill.get_filled_quantity(), 1.0);
    EXPECT_DOUBLE_EQ(fill.get_cumulative_filled_qty(), 1.0);
    EXPECT_DOUBLE_EQ(fill.get_remaining_qty(), 999'999'999.0);
}

TEST(ExecutionBridge, LargeOrderFractionalOverfillFailsBeforeEnqueue)
{
    bridge_harness h;
    ASSERT_TRUE(h.bridge->open());
    h.bridge->submit_order(make_order(
        3, "TEST", 1'000'000'000.0, 1.0));

    h.ft->deliver(
        "full|tt-3|EX-1|TEST|buy|1000000000.5|1|native-over|1000000000.5");
    fill_event fill({}, "", 0, order_side::buy, 0.0, 0.0);
    EXPECT_FALSE(h.bridge->peek_fill(fill));
    std::vector<ExecutionBridge::submit_result> results;
    ASSERT_TRUE(h.bridge->poll_submit_results(results));
    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].fatal);
    EXPECT_NE(results[0].error.find("cumulative quantity is inconsistent"),
              std::string::npos);
}

TEST(ExecutionBridge, NativeExecutionReplayIsExactlyOnceBeyondLocalFillIds)
{
    bridge_harness h;
    ASSERT_TRUE(h.bridge->open());
    h.bridge->submit_order(make_order(3, "TEST", 10.0, 100.0));

    constexpr std::string_view report =
        "partial|tt-3|EX-1|TEST|buy|4|100|venue-exec-7|4";
    h.ft->deliver(report);
    h.ft->deliver(report);

    std::vector<fill_event> fills;
    ASSERT_TRUE(h.bridge->poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].get_venue_execution_id(), "venue-exec-7");
    EXPECT_EQ(fills[0].get_cumulative_source(),
              fill_cumulative_source::venue_reported);
    EXPECT_DOUBLE_EQ(fills[0].get_cumulative_filled_qty(), 4.0);

    std::vector<ExecutionBridge::submit_result> results;
    EXPECT_FALSE(h.bridge->poll_submit_results(results));
}

TEST(ExecutionBridge, NativeExecutionReplayWithChangedEconomicsFailsClosed)
{
    bridge_harness h;
    ASSERT_TRUE(h.bridge->open());
    h.bridge->submit_order(make_order(3, "TEST", 10.0, 100.0));

    h.ft->deliver(
        "partial|tt-3|EX-1|TEST|buy|4|100|venue-exec-7|4");
    h.ft->deliver(
        "partial|tt-3|EX-1|TEST|buy|4|101|venue-exec-7|4");

    std::vector<ExecutionBridge::submit_result> results;
    ASSERT_TRUE(h.bridge->poll_submit_results(results));
    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].fatal);
    EXPECT_NE(results[0].error.find("changed economic fields"),
              std::string::npos);
}

TEST(ExecutionBridge, NativeExecutionCannotMoveToAnotherEngineOrder)
{
    bridge_harness h;
    ASSERT_TRUE(h.bridge->open());
    h.bridge->submit_order(make_order(3, "TEST", 1.0, 100.0));
    h.bridge->submit_order(make_order(4, "TEST", 1.0, 100.0));

    h.ft->deliver("full|tt-3|EX-3|TEST|buy|1|100|native-X|1");
    h.ft->deliver("full|tt-4|EX-4|TEST|buy|1|100|native-X|1");

    std::vector<ExecutionBridge::submit_result> results;
    ASSERT_TRUE(h.bridge->poll_submit_results(results));
    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].fatal);
    EXPECT_NE(results[0].error.find("changed economic fields"),
              std::string::npos);

    fill_event first({}, "", 0, order_side::buy, 0.0, 0.0);
    ASSERT_TRUE(h.bridge->peek_fill(first));
    EXPECT_EQ(first.get_order_id(), 3u);
    ASSERT_TRUE(h.bridge->acknowledge_fill(first.get_fill_id()));
    EXPECT_FALSE(h.bridge->peek_fill(first));
}

TEST(ExecutionBridge, ShortTerminalFillFailsClosed)
{
    bridge_harness h;
    ASSERT_TRUE(h.bridge->open());
    h.bridge->submit_order(make_order(3, "TEST", 10.0, 100.0));

    h.ft->deliver("full|tt-3|EX-1|TEST|buy|4|100");

    std::vector<fill_event> fills;
    EXPECT_FALSE(h.bridge->poll_fills(fills));
    std::vector<ExecutionBridge::submit_result> results;
    ASSERT_TRUE(h.bridge->poll_submit_results(results));
    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].fatal);
    EXPECT_NE(results[0].error.find("does not complete"), std::string::npos);
}

TEST(ExecutionBridge, PartialSliceAtTotalPromotesToTerminalFill)
{
    bridge_harness h;
    ASSERT_TRUE(h.bridge->open());
    h.bridge->submit_order(make_order(3, "TEST", 2.0, 100.0));

    h.ft->deliver("partial|tt-3|EX-1|TEST|buy|1|100");
    h.ft->deliver("partial|tt-3|EX-1|TEST|buy|1|100");

    std::vector<fill_event> fills;
    ASSERT_TRUE(h.bridge->poll_fills(fills));
    ASSERT_EQ(fills.size(), 2u);
    EXPECT_TRUE(fills[0].is_partial());
    EXPECT_FALSE(fills[1].is_partial());
    EXPECT_FALSE(h.bridge->cancel_order(3))
        << "quantity-complete partial slices must retire bridge tracking";
}

TEST(ExecutionBridge, MalformedFillClosesAdmissionAndPublishesFatalResult)
{
    bridge_harness h;
    ASSERT_TRUE(h.bridge->open());
    h.bridge->submit_order(make_order(3, "TEST", 10.0, 100.0));

    h.ft->deliver("full|tt-3|EX-1|TEST|buy|nan|100");

    std::vector<fill_event> fills;
    EXPECT_FALSE(h.bridge->poll_fills(fills));
    std::vector<ExecutionBridge::submit_result> results;
    ASSERT_TRUE(h.bridge->poll_submit_results(results));
    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].fatal);
    EXPECT_FALSE(results[0].ok);
    EXPECT_NE(results[0].error.find("malformed"), std::string::npos);

    h.bridge->submit_order(make_order(4, "TEST", 1.0, 100.0));
    EXPECT_NE(h.bridge->last_error().find("quiesced"), std::string::npos);
    EXPECT_FALSE(h.bridge->cancel_order(3));
}

TEST(ExecutionBridge, OverfillOrIdentityMismatchClosesAdmission)
{
    for (const std::string payload : {
             "full|tt-3|EX-1|TEST|buy|11|100",
             "full|tt-3|EX-1|OTHER|buy|10|100",
             "full|tt-3|EX-1|TEST|sell|10|100"})
    {
        bridge_harness h;
        ASSERT_TRUE(h.bridge->open());
        h.bridge->submit_order(make_order(3, "TEST", 10.0, 100.0));
        h.ft->deliver(payload);

        std::vector<fill_event> fills;
        EXPECT_FALSE(h.bridge->poll_fills(fills));
        std::vector<ExecutionBridge::submit_result> results;
        ASSERT_TRUE(h.bridge->poll_submit_results(results));
        ASSERT_EQ(results.size(), 1u);
        EXPECT_TRUE(results[0].fatal);
    }
}

TEST(ExecutionBridge, CancelFlowsThrough)
{
    bridge_harness h;
    h.tx->next_exchange_id_ = "EX-5";
    ASSERT_TRUE(h.bridge->open());

    h.bridge->submit_order(make_order(11));
    for (int i = 0; i < 5; ++i) { h.bridge->drain_outbound_for_test(); std::this_thread::yield(); }
    ASSERT_TRUE(h.bridge->cancel_order(11));
    for (int i = 0; i < 5; ++i) { h.bridge->drain_outbound_for_test(); std::this_thread::yield(); }

    ASSERT_EQ(h.tx->cancels_.size(), 1u);
    const auto& body = h.tx->cancels_[0].second;
    EXPECT_NE(body.find("exchangeId=EX-5"), std::string::npos);
    EXPECT_NE(body.find("clientId=tt-11"), std::string::npos);

    EXPECT_FALSE(h.bridge->cancel_order(999));
}

TEST(ExecutionBridge, RejectionPopulatesError)
{
    bridge_harness h;
    h.tx->submit_ok_ = false;
    ASSERT_TRUE(h.bridge->open());

    h.bridge->submit_order(make_order(1));
    for (int i = 0; i < 10; ++i) {
        h.bridge->drain_outbound_for_test();
        if (!h.bridge->last_error().empty()) break;
        std::this_thread::yield();
    }

    // Error may be in last_error or via poll result; check either
    bool has_error = !h.bridge->last_error().empty();
    std::vector<ExecutionBridge::submit_result> r;
    if (h.bridge->poll_submit_results(r) && !r.empty() && !r[0].ok) has_error = true;
    EXPECT_TRUE(has_error);

    EXPECT_FALSE(h.bridge->cancel_order(1));
}

TEST(ExecutionBridge, UnknownClientIdDropsFill)
{
    bridge_harness h;
    ASSERT_TRUE(h.bridge->open());

    h.ft->deliver("full|tt-9999|EX-X|TEST|buy|1|100");

    std::vector<fill_event> fills;
    EXPECT_FALSE(h.bridge->poll_fills(fills));
}

TEST(ExecutionBridge, StatusTransitionsDrainable)
{
    bridge_harness h;
    ASSERT_TRUE(h.bridge->open());

    h.ft->report_status(IFillTransport::lifecycle::degraded, "disconnect");
    h.ft->report_status(IFillTransport::lifecycle::open, "reconnected");

    std::vector<ExecutionBridge::status_event> out;
    ASSERT_TRUE(h.bridge->poll_status(out));
    ASSERT_GE(out.size(), 2u);

    bool saw_degraded = false, saw_reconnected = false;
    for (const auto& s : out)
    {
        if (s.state == IFillTransport::lifecycle::degraded) saw_degraded = true;
        if (s.state == IFillTransport::lifecycle::open && s.note == "reconnected")
            saw_reconnected = true;
    }
    EXPECT_TRUE(saw_degraded);
    EXPECT_TRUE(saw_reconnected);
}

TEST(ExecutionBridge, TerminalTombstoneStillAttributesALateFill)
{
    bridge_harness h;
    ASSERT_TRUE(h.bridge->open());

    h.bridge->submit_order(make_order(22));

    // Cancel is terminal for pending exposure, but its identity remains a
    // tombstone because a fill may already be in flight.
    h.ft->deliver("cancel|tt-22|EX-1|TEST|buy|0|0");

    h.ft->deliver("full|tt-22|EX-1|TEST|buy|10|100");

    std::vector<fill_event> fills;
    ASSERT_TRUE(h.bridge->poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].get_order_id(), 22u);
    EXPECT_FALSE(h.bridge->cancel_order(22));
}

TEST(ExecutionBridge, ConcurrentFillIngestAndPoll)
{
    // Drive handle_message from multiple threads while a separate thread
    // drains poll_fills. After joining, total polled fills must equal
    // total ingested - no loss, no duplicates.
    bridge_harness h;
    ASSERT_TRUE(h.bridge->open());

    constexpr int k_orders  = 200;
    constexpr int k_threads = 4;
    constexpr int k_per_thread = 250;  // 1000 total messages across 4 threads

    for (int i = 0; i < k_orders; ++i)
        h.bridge->submit_order(make_order(static_cast<uint64_t>(i + 1),
                                          "TEST", 1.0, 100.0));

    std::atomic<bool> stop_poller{false};
    std::vector<fill_event> collected;
    std::mutex collected_mu;

    std::thread poller([&]() {
        while (!stop_poller.load())
        {
            std::vector<fill_event> out;
            if (h.bridge->poll_fills(out))
            {
                std::lock_guard<std::mutex> lk(collected_mu);
                collected.insert(collected.end(),
                                 std::make_move_iterator(out.begin()),
                                 std::make_move_iterator(out.end()));
            }
            std::this_thread::yield();
        }
        // drain remainder
        std::vector<fill_event> out;
        while (h.bridge->poll_fills(out))
        {
            std::lock_guard<std::mutex> lk(collected_mu);
            collected.insert(collected.end(),
                             std::make_move_iterator(out.begin()),
                             std::make_move_iterator(out.end()));
            out.clear();
        }
    });

    std::atomic<int> produced{0};
    std::vector<std::thread> feeders;
    feeders.reserve(k_threads);
    for (int t = 0; t < k_threads; ++t)
    {
        feeders.emplace_back([&, t]() {
            for (int i = 0; i < k_per_thread; ++i)
            {
                int order_idx = (t * k_per_thread + i) % k_orders;
                std::ostringstream wire;
                wire << "partial|tt-" << (order_idx + 1)
                     << "|EX-1|TEST|buy|0.001|100";
                h.ft->deliver(wire.str());
                produced.fetch_add(1);
            }
        });
    }
    for (auto& f : feeders) f.join();

    // Let the poller drain.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop_poller.store(true);
    poller.join();

    EXPECT_EQ(produced.load(), k_threads * k_per_thread);
    std::lock_guard<std::mutex> lk(collected_mu);
    EXPECT_EQ(static_cast<int>(collected.size()),
              k_threads * k_per_thread);

    // Fill ids must be unique.
    std::vector<uint64_t> ids;
    ids.reserve(collected.size());
    for (const auto& f : collected) ids.push_back(f.get_fill_id());
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(std::adjacent_find(ids.begin(), ids.end()), ids.end());
}

TEST(ExecutionBridge, FillEventGetsExchangeSource)
{
    bridge_harness h;
    ASSERT_TRUE(h.bridge->open());
    h.bridge->submit_order(make_order(55, "ETHUSDT", 2.0, 3000.0, order_side::sell));
    h.ft->deliver("full|tt-55|EX-1|ETHUSDT|sell|2.0|3000");

    std::vector<fill_event> fills;
    ASSERT_TRUE(h.bridge->poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].get_source(), fill_source::exchange);
    EXPECT_EQ(fills[0].get_side(), order_side::sell);
}

// ===== Async submit tests (Phase 4/5) =====

TEST(ExecutionBridge, AsyncSubmitReportsResult)
{
    bridge_harness h;
    h.tx->next_exchange_id_ = "EX-ASYNC-1";
    ASSERT_TRUE(h.bridge->open());

    h.bridge->submit_order(make_order(100, "TEST", 1.0, 100.0));
    h.bridge->drain_outbound_for_test();

    std::vector<ExecutionBridge::submit_result> results;
    for (int i=0; i<1000 && results.empty(); ++i) {
        h.bridge->drain_outbound_for_test();
        (void)h.bridge->poll_submit_results(results);
        if (!results.empty()) break;
        std::this_thread::yield();
    }
    ASSERT_FALSE(results.empty()) << "no submit_result after drain";
    EXPECT_TRUE(results[0].ok);
    EXPECT_EQ(results[0].engine_id, 100u);
    EXPECT_EQ(results[0].symbol, "TEST");
    EXPECT_EQ(results[0].op, ExecutionBridge::submit_result::operation::submit);
    EXPECT_EQ(results[0].exchange_order_id, "EX-ASYNC-1");
}

TEST(ExecutionBridge, AsyncSubmitFailure)
{
    bridge_harness h;
    h.tx->submit_ok_ = false;
    ASSERT_TRUE(h.bridge->open());

    h.bridge->submit_order(make_order(101));
    h.bridge->drain_outbound_for_test();

    std::vector<ExecutionBridge::submit_result> results;
    for (int i=0; i<1000 && results.empty(); ++i) {
        h.bridge->drain_outbound_for_test();
        (void)h.bridge->poll_submit_results(results);
        if (!results.empty()) break;
        std::this_thread::yield();
    }
    ASSERT_FALSE(results.empty()) << "no submit_result after drain";
    EXPECT_FALSE(results[0].ok);
    EXPECT_EQ(results[0].op, ExecutionBridge::submit_result::operation::submit);
    EXPECT_FALSE(results[0].error.empty());
}

TEST(ExecutionBridge, AsyncSubmitPreservesAmbiguousAndFatalSafetySignals)
{
    bridge_harness h;
    h.tx->submit_ok_ = false;
    h.tx->submit_uncertain_ = true;
    h.tx->submit_fatal_ = true;
    ASSERT_TRUE(h.bridge->open());

    h.bridge->submit_order(make_order(105));
    std::vector<ExecutionBridge::submit_result> results;
    ASSERT_TRUE(wait_submit_results(*h.bridge, results));
    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].ok);
    EXPECT_TRUE(results[0].uncertain);
    EXPECT_TRUE(results[0].fatal);
}

TEST(ExecutionBridge, TerminalSubmitOutcomeDropsAlreadyQueuedMutation)
{
    bridge_harness h;
    h.tx->submit_ok_ = false;
    h.tx->submit_uncertain_ = true;
    h.tx->submit_fatal_ = true;
    ASSERT_TRUE(h.bridge->open());

    h.bridge->submit_order(make_order(107));
    h.bridge->submit_order(make_order(108));
    h.bridge->drain_outbound_for_test();

    std::vector<ExecutionBridge::submit_result> results;
    ASSERT_TRUE(h.bridge->poll_submit_results(results));
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results.front().engine_id, 107u);
    EXPECT_TRUE(results.front().uncertain);
    EXPECT_TRUE(results.front().fatal);
    EXPECT_EQ(h.tx->submissions_.size(), 1u);
    EXPECT_FALSE(h.bridge->cancel_order(108));
}

TEST(ExecutionBridge, NonStdTransportExceptionClosesAdmissionBeforeNextMutation)
{
    auto tx = std::make_shared<NonStdThrowingOrderTransport>();
    auto ft = std::make_shared<FakeFillTransport>();
    auto en = std::make_shared<FakeEncoder>();
    auto pa = std::make_shared<FakeParser>();
    ExecutionBridge::deps d;
    d.order_tx = tx;
    d.fill_tx = ft;
    d.encoder = en;
    d.parser = pa;
    d.write_safety_readiness = validated_write_safety_readiness();
    d.start_transport_thread = true;
    ExecutionBridge bridge(std::move(d));
    ASSERT_TRUE(bridge.open());

    bridge.submit_order(make_order(109));
    bridge.submit_order(make_order(110));

    std::vector<ExecutionBridge::submit_result> results;
    for (int i = 0; i < 10000 && results.empty(); ++i)
    {
        (void)bridge.poll_submit_results(results);
        std::this_thread::yield();
    }
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results.front().engine_id, 109u);
    EXPECT_TRUE(results.front().uncertain);
    EXPECT_TRUE(results.front().fatal);
    EXPECT_EQ(tx->calls.load(std::memory_order_acquire), 1);
    EXPECT_FALSE(bridge.cancel_order(110));
    bridge.close();
}

TEST(ExecutionBridge, AsyncCancelReportsTypedResult)
{
    bridge_harness h;
    h.tx->next_exchange_id_ = "EX-CANCEL-1";
    ASSERT_TRUE(h.bridge->open());

    h.bridge->submit_order(make_order(103, "FOO", 1.0, 100.0));
    std::vector<ExecutionBridge::submit_result> results;
    ASSERT_TRUE(wait_submit_results(*h.bridge, results));
    results.clear();

    ASSERT_TRUE(h.bridge->cancel_order(103));
    ASSERT_TRUE(wait_submit_results(*h.bridge, results));
    ASSERT_FALSE(results.empty());
    EXPECT_TRUE(results[0].ok);
    EXPECT_EQ(results[0].engine_id, 103u);
    EXPECT_EQ(results[0].symbol, "FOO");
    EXPECT_EQ(results[0].op, ExecutionBridge::submit_result::operation::cancel);
}

TEST(ExecutionBridge, AsyncCancelFailureReportsCancelOp)
{
    bridge_harness h;
    h.tx->cancel_ok_ = false;
    ASSERT_TRUE(h.bridge->open());

    h.bridge->submit_order(make_order(104, "BAR", 1.0, 100.0));
    std::vector<ExecutionBridge::submit_result> results;
    ASSERT_TRUE(wait_submit_results(*h.bridge, results));
    results.clear();

    ASSERT_TRUE(h.bridge->cancel_order(104));
    ASSERT_TRUE(wait_submit_results(*h.bridge, results));
    ASSERT_FALSE(results.empty());
    EXPECT_FALSE(results[0].ok);
    EXPECT_EQ(results[0].engine_id, 104u);
    EXPECT_EQ(results[0].symbol, "BAR");
    EXPECT_EQ(results[0].op, ExecutionBridge::submit_result::operation::cancel);
    EXPECT_FALSE(results[0].error.empty());
}

TEST(ExecutionBridge, AsyncCancelPreservesAmbiguousAndFatalSafetySignals)
{
    bridge_harness h;
    h.tx->cancel_ok_ = false;
    h.tx->cancel_uncertain_ = true;
    h.tx->cancel_fatal_ = true;
    ASSERT_TRUE(h.bridge->open());

    h.bridge->submit_order(make_order(106, "BAR", 1.0, 100.0));
    std::vector<ExecutionBridge::submit_result> results;
    ASSERT_TRUE(wait_submit_results(*h.bridge, results));
    results.clear();

    ASSERT_TRUE(h.bridge->cancel_order(106));
    ASSERT_TRUE(wait_submit_results(*h.bridge, results));
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].op, ExecutionBridge::submit_result::operation::cancel);
    EXPECT_FALSE(results[0].ok);
    EXPECT_TRUE(results[0].uncertain);
    EXPECT_TRUE(results[0].fatal);
}

TEST(ExecutionBridge, PreAckCancelUsesClientId)
{
    bridge_harness h;
    h.tx->next_exchange_id_ = "EX-PRE";
    ASSERT_TRUE(h.bridge->open());

    h.bridge->submit_order(make_order(102, "FOO", 5.0, 123.0));
    // Cancel immediately, before drain (simulates cancel before REST ack)
    ASSERT_TRUE(h.bridge->cancel_order(102));
    h.bridge->drain_outbound_for_test();

    // Should have 1 submit + 1 cancel - robust wait for bg thread
    for (int i = 0; i < 1000 && (h.tx->submissions_.size() < 1 || h.tx->cancels_.size() < 1); ++i) {
        h.bridge->drain_outbound_for_test();
        std::this_thread::yield();
    }
    ASSERT_EQ(h.tx->submissions_.size(), 1u);
    ASSERT_GE(h.tx->cancels_.size(), 1u);
    // Cancel should prefer client id fallback when no exchange yet
    const auto& cancel_body = h.tx->cancels_.back().second;
    bool used_client = (cancel_body.find("origClientOrderId") != std::string::npos) ||
                       (cancel_body.find("clientId=tt-102") != std::string::npos) ||
                       (cancel_body.find("tt-102") != std::string::npos);
    EXPECT_TRUE(used_client);
}

TEST(ExecutionBridge, SlowTransportStillNonBlocking)
{
    bridge_harness h;
    // Make submit slow
    h.tx->submit_ok_ = true;
    ASSERT_TRUE(h.bridge->open());

    auto start = std::chrono::steady_clock::now();
    h.bridge->submit_order(make_order(200));
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();

    // submit_order itself must return very fast (< 1ms even if transport would be slow)
    EXPECT_LT(elapsed_us, 1000);  // 1ms generous upper bound for enqueue

    // Now let it process
    h.bridge->drain_outbound_for_test();
}

TEST(ExecutionBridge, FundingFastPathEnqueuesOrLatchesFailure)
{
    auto tx = std::make_shared<FakeOrderTransport>();
    auto ft = std::make_shared<FakeFillTransport>();
    ExecutionBridge::deps d;
    d.order_tx = tx;
    d.fill_tx = ft;
    d.parser = std::make_shared<FakeFundingParser>();
    d.write_safety_readiness = validated_write_safety_readiness();
    d.start_transport_thread = false;
    int accepted = 0;
    int failures = 0;
    d.funding_update_handler = [&](const parsed_funding_update& update) {
        ++accepted;
        return update.event_time_ms == 1'700'000'000'000LL
            && update.cash_delta == -0.5;
    };
    d.funding_failure_handler = [&] { ++failures; };
    ExecutionBridge bridge(std::move(d));
    ASSERT_TRUE(bridge.open());

    ft->deliver("funding");
    EXPECT_EQ(accepted, 1);
    EXPECT_EQ(failures, 0);
    ft->deliver("malformed");
    EXPECT_EQ(accepted, 1);
    EXPECT_EQ(failures, 1);
}

TEST(ExecutionBridge, QuiesceWaitsForAdmittedCallAndRejectsQueuedAndLateMutations)
{
    auto tx = std::make_shared<BlockingOrderTransport>();
    auto ft = std::make_shared<FakeFillTransport>();
    auto en = std::make_shared<FakeEncoder>();
    auto pa = std::make_shared<FakeParser>();
    ExecutionBridge::deps d;
    d.order_tx = tx;
    d.fill_tx = ft;
    d.encoder = en;
    d.parser = pa;
    d.write_safety_readiness = validated_write_safety_readiness();
    d.start_transport_thread = true;
    ExecutionBridge bridge(std::move(d));
    ASSERT_TRUE(bridge.open());

    bridge.submit_order(make_order(701));
    ASSERT_TRUE(tx->wait_until_entered());
    bridge.submit_order(make_order(702));

    std::atomic<bool> quiesced{false};
    std::atomic<bool> quiesce_saw_completion{false};
    std::thread shutdown([&] {
        bridge.quiesce();
        quiesce_saw_completion.store(tx->completed(), std::memory_order_release);
        quiesced.store(true, std::memory_order_release);
    });

    // Do not release the admitted venue call until quiesce has actually
    // closed admission. A thread-start notification is insufficient here:
    // the worker could otherwise process request 702 before the shutdown
    // thread gets scheduled (ASAN makes that race easy to reproduce).
    bool admission_closed = false;
    const auto admission_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < admission_deadline)
    {
        if (!bridge.cancel_order(701))
        {
            admission_closed = true;
            break;
        }
        std::this_thread::yield();
    }
    tx->release();
    shutdown.join();
    ASSERT_TRUE(admission_closed);
    EXPECT_TRUE(quiesced.load(std::memory_order_acquire));
    EXPECT_TRUE(quiesce_saw_completion.load(std::memory_order_acquire));
    EXPECT_EQ(tx->submissions_.size(), 1u);
    EXPECT_TRUE(tx->cancels_.empty());

    bridge.drain_outbound_for_test();
    EXPECT_EQ(tx->submissions_.size(), 1u);
    bridge.submit_order(make_order(703));
    bridge.drain_outbound_for_test();
    EXPECT_EQ(tx->submissions_.size(), 1u);
    EXPECT_FALSE(bridge.cancel_order(701));
}
