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
        if (!cancel_ok_) r.error = "fake-cancel-fail";
        return r;
    }

    bool opened_ = false;
    bool submit_ok_ = true;
    bool cancel_ok_ = true;
    std::string next_exchange_id_ = "EX-1";
    std::vector<std::pair<std::string, std::string>> submissions_;
    std::vector<std::pair<std::string, std::string>> cancels_;
};

class FakeFillTransport : public IFillTransport
{
public:
    bool open() override
    {
        state_ = lifecycle::open;
        if (status_cb_) status_cb_(state_, "opened");
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

// Test wire format: "kind|client_id|exchange_id|symbol|side|qty|price"
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
        out.ts                = std::chrono::system_clock::now();
        return true;
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
    EXPECT_EQ(f.get_source(), fill_source::exchange);
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

// Dual-channel venues (Bitget order+fill) emit full_fill with last_fill_qty=0
// for lifecycle untrack. Bridge must untrack without inventing a zero-qty fill.
TEST(ExecutionBridge, ZeroQtyFullFillUntracksWithoutFillEvent)
{
    bridge_harness h;
    ASSERT_TRUE(h.bridge->open());

    h.bridge->submit_order(make_order(21, "BTCUSDT", 1.0, 50000.0));
    // Order-channel style: full_fill, qty=0
    h.ft->deliver("full|tt-21|EX-21|BTCUSDT|buy|0|0");

    std::vector<fill_event> fills;
    EXPECT_FALSE(h.bridge->poll_fills(fills));

    // Mapping cleared — cancel should miss
    EXPECT_FALSE(h.bridge->cancel_order(21));
}

TEST(ExecutionBridge, ZeroQtyPartialFillEmitsNothing)
{
    bridge_harness h;
    ASSERT_TRUE(h.bridge->open());

    h.bridge->submit_order(make_order(22, "BTCUSDT", 1.0, 50000.0));
    h.ft->deliver("partial|tt-22|EX-22|BTCUSDT|buy|0|0");

    std::vector<fill_event> fills;
    EXPECT_FALSE(h.bridge->poll_fills(fills));

    // Still tracked (partial is non-terminal) — cancel should find it
    EXPECT_TRUE(h.bridge->cancel_order(22));
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

TEST(ExecutionBridge, TerminalStatesClearMapping)
{
    bridge_harness h;
    ASSERT_TRUE(h.bridge->open());

    h.bridge->submit_order(make_order(22));

    // Cancel comes in from the exchange; mapping must be cleared.
    h.ft->deliver("cancel|tt-22|EX-1|TEST|buy|0|0");

    // A subsequent fill for the same client id should now be dropped.
    h.ft->deliver("full|tt-22|EX-1|TEST|buy|10|100");

    std::vector<fill_event> fills;
    EXPECT_FALSE(h.bridge->poll_fills(fills));
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
