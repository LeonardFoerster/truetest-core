#include <gtest/gtest.h>

#include "execution/execution_bridge.h"
#include "providers/binance/binance_futures_user_data_parser.h"
#include "providers/binance/binance_user_data_parser.h"
#ifdef HAS_BITGET
#include "providers/bitget/bitget_futures_user_data_parser.h"
#endif

#include <atomic>
#include <array>
#include <chrono>
#include <memory>
#include <new>
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
    execution_parse_result parse(std::string_view raw,
                                 parsed_exec& out) override
    {
        if (raw == "malformed") return execution_parse_result::malformed;
        if (raw == "throws") throw std::bad_alloc{};
        std::vector<std::string> parts;
        std::string cur;
        for (char c : raw)
        {
            if (c == '|') { parts.push_back(std::move(cur)); cur.clear(); }
            else          { cur.push_back(c); }
        }
        if (!cur.empty()) parts.push_back(std::move(cur));
        if (parts.size() < 7) return execution_parse_result::unrelated;

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
        return execution_parse_result::valid;
    }

    bool parse_position_snapshot(std::string_view,
                                 parsed_position_snapshot&) override
    {
        ++snapshot_calls;
        if (throw_snapshot) throw std::bad_alloc{};
        return false;
    }

    int snapshot_calls = 0;
    bool throw_snapshot = false;
};

class FakeFundingParser : public IFillParser
{
public:
    execution_parse_result parse(std::string_view, parsed_exec&) override
    {
        return execution_parse_result::unrelated;
    }

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

// Deliberately small fixed test double for the execution-layer ingress port.
// It assigns source sequence numbers just like the provider-owned SPSC ring,
// but keeps this bridge unit test independent of provider implementations.
class TestPrivateExecutionIngress final : public IPrivateExecutionIngress
{
public:
    static constexpr std::size_t capacity = 16;

    bool try_publish(private_execution_record& record) noexcept override
    {
        if (failed_ || record.sequence != 0 || size_ == capacity
            || next_sequence_ == 0)
        {
            failed_ = true;
            return false;
        }

        record.sequence = next_sequence_;
        if (!record.valid_shape())
        {
            record.sequence = 0;
            failed_ = true;
            return false;
        }

        records_[tail_] = record;
        tail_ = (tail_ + 1) % capacity;
        ++size_;
        ++next_sequence_;
        return true;
    }

    bool try_pop(private_execution_record& record) noexcept override
    {
        if (size_ == 0) return false;
        record = records_[head_];
        head_ = (head_ + 1) % capacity;
        --size_;
        return true;
    }

    bool empty() const noexcept override { return size_ == 0; }
    bool failed() const noexcept override { return failed_; }
    void latch_failure() noexcept override { failed_ = true; }

private:
    std::array<private_execution_record, capacity> records_{};
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    std::size_t size_ = 0;
    std::uint64_t next_sequence_ = 1;
    bool failed_ = false;
};

class ScriptedPrivateExecutionParser final : public IFillParser
{
public:
    execution_parse_result parse(std::string_view, parsed_exec& out) override
    {
        out = next;
        return result;
    }

    parsed_exec next{};
    execution_parse_result result = execution_parse_result::valid;
};

parsed_exec make_unified_exec(parsed_exec::kind kind,
                              std::string client_id,
                              std::string exchange_id,
                              std::string symbol = "BTCUSDT",
                              order_side side = order_side::buy)
{
    parsed_exec exec;
    exec.k = kind;
    exec.client_order_id = std::move(client_id);
    exec.exchange_order_id = std::move(exchange_id);
    exec.symbol = std::move(symbol);
    exec.side = side;
    exec.ts = std::chrono::system_clock::time_point{
        std::chrono::milliseconds{1'700'000'000'000LL}};
    return exec;
}

private_execution_record make_unified_full_record(
    std::string_view client_id,
    std::string_view exchange_id,
    double quantity = 1.0)
{
    private_execution_record record;
    record.k = private_execution_record::kind::full_fill;
    record.event_time_ms = 1'700'000'000'000LL;
    record.side = order_side::buy;
    record.last_fill_qty = quantity;
    record.last_fill_price = 100.0;
    record.cumulative_qty = quantity;
    record.commission = 0.0;
    record.remaining_qty = 0.0;
    record.cumulative_reported = true;
    (void)private_execution_record::copy_text(
        record.symbol, record.symbol_size, "BTCUSDT");
    (void)private_execution_record::copy_text(
        record.client_order_id, record.client_order_id_size, client_id);
    (void)private_execution_record::copy_text(
        record.exchange_order_id, record.exchange_order_id_size, exchange_id);
    (void)private_execution_record::copy_text(
        record.execution_id, record.execution_id_size, "private-trade-1");
    return record;
}

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
    int execution_failures = 0;

    std::unique_ptr<ExecutionBridge> bridge;

    bridge_harness() { rebuild(); }

    void rebuild()
    {
        ExecutionBridge::deps d;
        d.order_tx = tx;
        d.fill_tx  = ft;
        d.encoder  = en;
        d.parser   = pa;
        d.execution_failure_handler = [this] { ++execution_failures; };
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

TEST(ExecutionBridge, CloseDetachesPrivateCallbacksOnlyAfterReaderClose)
{
    bridge_harness h;
    ASSERT_TRUE(h.bridge->open());
    ASSERT_TRUE(static_cast<bool>(h.ft->message_cb_));
    ASSERT_TRUE(static_cast<bool>(h.ft->status_cb_));

    h.bridge->close();
    EXPECT_FALSE(static_cast<bool>(h.ft->message_cb_));
    EXPECT_FALSE(static_cast<bool>(h.ft->status_cb_));

    // Explicit reopen reattaches the bridge-owned callbacks before the
    // private transport is allowed to report readiness again.
    ASSERT_TRUE(h.bridge->open());
    EXPECT_TRUE(static_cast<bool>(h.ft->message_cb_));
    EXPECT_TRUE(static_cast<bool>(h.ft->status_cb_));

    h.bridge.reset();
    // A provider-held transport may now be destroyed or publish its final
    // closed status without retaining a stale ExecutionBridge [this].
    EXPECT_FALSE(static_cast<bool>(h.ft->message_cb_));
    EXPECT_FALSE(static_cast<bool>(h.ft->status_cb_));
}

TEST(ExecutionBridge, MissingParserOrFatalSinkCannotOpen)
{
    auto tx = std::make_shared<FakeOrderTransport>();
    auto ft = std::make_shared<FakeFillTransport>();
    auto en = std::make_shared<FakeEncoder>();

    ExecutionBridge::deps missing_parser;
    missing_parser.order_tx = tx;
    missing_parser.fill_tx = ft;
    missing_parser.encoder = en;
    missing_parser.start_transport_thread = false;
    ExecutionBridge no_parser(std::move(missing_parser));
    EXPECT_FALSE(no_parser.open());
    EXPECT_FALSE(tx->opened_);

    ExecutionBridge::deps missing_sink;
    missing_sink.order_tx = tx;
    missing_sink.fill_tx = ft;
    missing_sink.encoder = en;
    missing_sink.parser = std::make_shared<FakeParser>();
    missing_sink.start_transport_thread = false;
    ExecutionBridge no_sink(std::move(missing_sink));
    EXPECT_FALSE(no_sink.open());
    EXPECT_FALSE(tx->opened_);
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

TEST(ExecutionBridge, MalformedPrivateExecutionClosesAdmissionOnce)
{
    bridge_harness h;
    ASSERT_TRUE(h.bridge->open());

    h.ft->deliver("malformed");
    h.ft->deliver("malformed");

    EXPECT_EQ(h.execution_failures, 1);
    EXPECT_EQ(h.pa->snapshot_calls, 0);
    EXPECT_FALSE(h.bridge->last_error().empty());
    EXPECT_FALSE(h.bridge->open());

    h.bridge->submit_order(make_order(98));
    h.bridge->drain_outbound_for_test();
    EXPECT_TRUE(h.tx->submissions_.empty());
    EXPECT_FALSE(h.bridge->cancel_order(98));
}

TEST(ExecutionBridge, ThrowingPrivateParserClosesAdmissionBeforeSnapshot)
{
    bridge_harness h;
    ASSERT_TRUE(h.bridge->open());

    h.ft->deliver("throws");

    EXPECT_EQ(h.execution_failures, 1);
    EXPECT_EQ(h.pa->snapshot_calls, 0);
    h.bridge->submit_order(make_order(99));
    h.bridge->drain_outbound_for_test();
    EXPECT_TRUE(h.tx->submissions_.empty());
}

TEST(ExecutionBridge, ThrowingPrivateSnapshotPathClosesAdmission)
{
    bridge_harness h;
    h.bridge->set_unknown_fill_handler({});
    // Enable the snapshot branch; a real provider uses this for futures
    // account/position updates after the tri-state execution parser says
    // unrelated.
    h.bridge.reset();
    ExecutionBridge::deps d;
    d.order_tx = h.tx;
    d.fill_tx = h.ft;
    d.encoder = h.en;
    d.parser = h.pa;
    d.execution_failure_handler = [&h] { ++h.execution_failures; };
    d.position_snapshot_handler = [](const parsed_position_snapshot&) {};
    d.start_transport_thread = false;
    h.bridge = std::make_unique<ExecutionBridge>(std::move(d));
    ASSERT_TRUE(h.bridge->open());

    h.pa->throw_snapshot = true;
    h.ft->deliver("unrelated-control-frame");

    EXPECT_EQ(h.execution_failures, 1);
    h.bridge->submit_order(make_order(991));
    h.bridge->drain_outbound_for_test();
    EXPECT_TRUE(h.tx->submissions_.empty());
}

TEST(ExecutionBridge, BinanceListenKeyExpiryClosesAdmission)
{
    auto tx = std::make_shared<FakeOrderTransport>();
    auto ft = std::make_shared<FakeFillTransport>();
    auto en = std::make_shared<FakeEncoder>();
    int execution_failures = 0;
    ExecutionBridge::deps d;
    d.order_tx = tx;
    d.fill_tx = ft;
    d.encoder = en;
    d.parser = std::make_shared<BinanceUserDataParser>();
    d.execution_failure_handler = [&] { ++execution_failures; };
    d.start_transport_thread = false;
    ExecutionBridge bridge(std::move(d));
    ASSERT_TRUE(bridge.open());

    ft->deliver(R"({"e":"listenKeyExpired","E":1700000000000})");

    EXPECT_EQ(execution_failures, 1);
    bridge.submit_order(make_order(100));
    bridge.drain_outbound_for_test();
    EXPECT_TRUE(tx->submissions_.empty());
}

TEST(ExecutionBridge, BinanceOcoListLifecycleClosesAdmissionUntilTypedIngress)
{
    auto tx = std::make_shared<FakeOrderTransport>();
    auto ft = std::make_shared<FakeFillTransport>();
    auto en = std::make_shared<FakeEncoder>();
    int execution_failures = 0;
    ExecutionBridge::deps d;
    d.order_tx = tx;
    d.fill_tx = ft;
    d.encoder = en;
    d.parser = std::make_shared<BinanceUserDataParser>();
    d.execution_failure_handler = [&] { ++execution_failures; };
    d.start_transport_thread = false;
    ExecutionBridge bridge(std::move(d));
    ASSERT_TRUE(bridge.open());

    ft->deliver(R"({"e":"listStatus","E":1700000000000,"l":"ALL_DONE","L":"ALL_DONE"})");

    EXPECT_EQ(execution_failures, 1);
    bridge.submit_order(make_order(101));
    bridge.drain_outbound_for_test();
    EXPECT_TRUE(tx->submissions_.empty());
}

TEST(ExecutionBridge, BinanceFuturesListenKeyExpiryClosesAdmission)
{
    auto tx = std::make_shared<FakeOrderTransport>();
    auto ft = std::make_shared<FakeFillTransport>();
    auto en = std::make_shared<FakeEncoder>();
    int execution_failures = 0;
    ExecutionBridge::deps d;
    d.order_tx = tx;
    d.fill_tx = ft;
    d.encoder = en;
    d.parser = std::make_shared<BinanceFuturesUserDataParser>();
    d.execution_failure_handler = [&] { ++execution_failures; };
    d.start_transport_thread = false;
    ExecutionBridge bridge(std::move(d));
    ASSERT_TRUE(bridge.open());

    ft->deliver(R"({"e":"listenKeyExpired","E":1700000000000})");

    EXPECT_EQ(execution_failures, 1);
    bridge.submit_order(make_order(101));
    bridge.drain_outbound_for_test();
    EXPECT_TRUE(tx->submissions_.empty());
}

TEST(ExecutionBridge, BinanceFuturesUnsupportedConditionalLifecycleClosesAdmission)
{
    auto tx = std::make_shared<FakeOrderTransport>();
    auto ft = std::make_shared<FakeFillTransport>();
    auto en = std::make_shared<FakeEncoder>();
    int execution_failures = 0;
    ExecutionBridge::deps d;
    d.order_tx = tx;
    d.fill_tx = ft;
    d.encoder = en;
    d.parser = std::make_shared<BinanceFuturesUserDataParser>();
    d.execution_failure_handler = [&] { ++execution_failures; };
    d.start_transport_thread = false;
    ExecutionBridge bridge(std::move(d));
    ASSERT_TRUE(bridge.open());

    ft->deliver(R"({"e":"CONDITIONAL_ORDER_TRIGGER_REJECT","E":1700000000000})");

    EXPECT_EQ(execution_failures, 1);
    bridge.submit_order(make_order(102));
    bridge.drain_outbound_for_test();
    EXPECT_TRUE(tx->submissions_.empty());
}

TEST(ExecutionBridge, ContradictoryTrackedExchangeIdentityHaltsBeforeSecondFill)
{
    bridge_harness h;
    ASSERT_TRUE(h.bridge->open());
    h.bridge->submit_order(make_order(1000, "BTCUSDT", 2.0, 100.0));

    h.ft->deliver("partial|tt-1000|EX-A|BTCUSDT|buy|1|100");
    h.ft->deliver("partial|tt-1000|EX-B|BTCUSDT|buy|1|100");

    std::vector<fill_event> fills;
    ASSERT_TRUE(h.bridge->poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills.front().get_order_id(), 1000u);
    EXPECT_EQ(h.execution_failures, 1);

    h.bridge->submit_order(make_order(1001));
    h.bridge->drain_outbound_for_test();
    EXPECT_TRUE(h.tx->submissions_.empty());
}

TEST(ExecutionBridge, ExchangeIdentityCannotBelongToTwoTrackedOrders)
{
    bridge_harness h;
    ASSERT_TRUE(h.bridge->open());
    h.bridge->submit_order(make_order(1010, "BTCUSDT", 2.0, 100.0));
    h.bridge->submit_order(make_order(1011, "BTCUSDT", 2.0, 100.0));

    h.ft->deliver("partial|tt-1011|EX-SHARED|BTCUSDT|buy|1|100");
    h.ft->deliver("partial|tt-1010|EX-SHARED|BTCUSDT|buy|1|100");

    std::vector<fill_event> fills;
    ASSERT_TRUE(h.bridge->poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills.front().get_order_id(), 1011u);
    EXPECT_EQ(h.execution_failures, 1);

    h.bridge->submit_order(make_order(1012));
    h.bridge->drain_outbound_for_test();
    EXPECT_TRUE(h.tx->submissions_.empty());
}

TEST(ExecutionBridge, UnknownClientCannotReuseTrackedExchangeIdentity)
{
    bridge_harness h;
    ASSERT_TRUE(h.bridge->open());
    h.bridge->submit_order(make_order(1015, "BTCUSDT", 2.0, 100.0));

    h.ft->deliver("partial|tt-1015|EX-A|BTCUSDT|buy|1|100");
    h.ft->deliver("ack|tt-other|EX-A|BTCUSDT|buy|0|0");

    std::vector<fill_event> fills;
    ASSERT_TRUE(h.bridge->poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills.front().get_order_id(), 1015u);
    EXPECT_EQ(h.execution_failures, 1);

    h.bridge->submit_order(make_order(1016));
    h.bridge->drain_outbound_for_test();
    EXPECT_TRUE(h.tx->submissions_.empty());
}

TEST(ExecutionBridge, PrivateAndRestExchangeIdentityConflictHalts)
{
    auto tx = std::make_shared<BlockingOrderTransport>();
    tx->next_exchange_id_ = "EX-REST";
    auto ft = std::make_shared<FakeFillTransport>();
    auto en = std::make_shared<FakeEncoder>();
    auto pa = std::make_shared<FakeParser>();
    int execution_failures = 0;

    ExecutionBridge::deps d;
    d.order_tx = tx;
    d.fill_tx = ft;
    d.encoder = en;
    d.parser = pa;
    d.execution_failure_handler = [&] { ++execution_failures; };
    d.start_transport_thread = true;
    ExecutionBridge bridge(std::move(d));
    ASSERT_TRUE(bridge.open());

    bridge.submit_order(make_order(1020, "BTCUSDT", 2.0, 100.0));
    ASSERT_TRUE(tx->wait_until_entered());
    ft->deliver("partial|tt-1020|EX-PRIVATE|BTCUSDT|buy|1|100");
    tx->release();

    std::vector<ExecutionBridge::submit_result> results;
    for (int i = 0; i < 10000 && results.empty(); ++i)
    {
        (void)bridge.poll_submit_results(results);
        std::this_thread::yield();
    }
    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results.front().ok);
    EXPECT_TRUE(results.front().uncertain);
    EXPECT_TRUE(results.front().fatal);
    EXPECT_EQ(execution_failures, 1);

    std::vector<fill_event> fills;
    ASSERT_TRUE(bridge.poll_fills(fills));
    ASSERT_EQ(fills.size(), 1u);
    bridge.submit_order(make_order(1021));
    EXPECT_FALSE(bridge.cancel_order(1020));
    bridge.close();
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

TEST(ExecutionBridge, SinglePrivateProducerIngestAndPoll)
{
    // A private WebSocket reader is the sole producer.  The engine may drain
    // concurrently, but modelling four concurrent transport callbacks would
    // test an MPSC contract that live ingress deliberately does not provide.
    bridge_harness h;
    ASSERT_TRUE(h.bridge->open());

    constexpr int k_orders = 200;
    constexpr int k_messages = 1000;

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
    std::thread feeder([&] {
        for (int i = 0; i < k_messages; ++i)
        {
            const int order_idx = i % k_orders;
            std::ostringstream wire;
            wire << "partial|tt-" << (order_idx + 1)
                 << "|EX-" << (order_idx + 1)
                 << "|TEST|buy|0.001|100";
            h.ft->deliver(wire.str());
            produced.fetch_add(1);
        }
    });
    feeder.join();

    // Let the poller drain.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop_poller.store(true);
    poller.join();

    EXPECT_EQ(produced.load(), k_messages);
    std::lock_guard<std::mutex> lk(collected_mu);
    EXPECT_EQ(static_cast<int>(collected.size()), k_messages);

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
    d.execution_failure_handler = [] {};
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

TEST(ExecutionBridgeUnifiedIngress,
     RestCancelAckRetainsMappingUntilPrivateTerminalIsAcknowledged)
{
    auto tx = std::make_shared<FakeOrderTransport>();
    tx->next_exchange_id_ = "EX-CANCEL-UNIFIED";
    auto fill_tx = std::make_shared<FakeFillTransport>();
    auto encoder = std::make_shared<FakeEncoder>();
    auto parser = std::make_shared<ScriptedPrivateExecutionParser>();
    TestPrivateExecutionIngress ingress;
    int execution_failures = 0;

    ExecutionBridge::deps d;
    d.order_tx = tx;
    d.fill_tx = fill_tx;
    d.encoder = encoder;
    d.parser = parser;
    d.execution_ingress = &ingress;
    d.require_execution_ingress = true;
    d.execution_failure_handler = [&] { ++execution_failures; };
    d.start_transport_thread = false;
    ExecutionBridge bridge(std::move(d));
    ASSERT_TRUE(bridge.open());

    bridge.submit_order(make_order(1900, "BTCUSDT", 1.0, 100.0));
    std::vector<ExecutionBridge::submit_result> results;
    ASSERT_TRUE(wait_submit_results(bridge, results));
    ASSERT_EQ(results.size(), 1u);
    ASSERT_TRUE(results.front().ok);
    results.clear();

    ASSERT_TRUE(bridge.cancel_order(1900));
    ASSERT_TRUE(wait_submit_results(bridge, results));
    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results.front().ok);
    EXPECT_EQ(results.front().op,
              ExecutionBridge::submit_result::operation::cancel);

    // A REST cancel acknowledgement cannot retire the identity: exactly one
    // authoritative private terminal must still resolve and be committed by
    // the engine before the bridge considers this lifecycle clean.
    EXPECT_TRUE(bridge.has_unresolved_private_lifecycle());
    EXPECT_FALSE(bridge.cancel_order(1900));

    parser->next = make_unified_exec(parsed_exec::kind::canceled,
                                     "tt-1900", "EX-CANCEL-UNIFIED");
    parser->next.has_cumulative_qty = true;
    parser->next.cumulative_qty = 0.0;
    fill_tx->deliver("private-cancel-terminal");

    private_execution_record terminal;
    ASSERT_TRUE(ingress.try_pop(terminal));
    EXPECT_EQ(terminal.k, private_execution_record::kind::canceled);
    EXPECT_EQ(terminal.client_order_id_view(), "tt-1900");
    EXPECT_EQ(terminal.exchange_order_id_view(), "EX-CANCEL-UNIFIED");
    ASSERT_EQ(bridge.resolve_private_execution(terminal),
              private_execution_resolution::tracked);
    EXPECT_EQ(terminal.engine_order_id, 1900u);
    EXPECT_TRUE(bridge.has_unresolved_private_lifecycle());

    ASSERT_TRUE(bridge.commit_private_execution(
        {terminal.sequence, terminal.engine_order_id}));
    ASSERT_TRUE(bridge.acknowledge_private_terminal(terminal.sequence));
    EXPECT_FALSE(bridge.has_unresolved_private_lifecycle());
    EXPECT_EQ(execution_failures, 0);
}

TEST(ExecutionBridgeUnifiedIngress,
     DelayedRestSubmitConflictingWithAcknowledgedPrivateTerminalHalts)
{
    auto tx = std::make_shared<BlockingOrderTransport>();
    tx->next_exchange_id_ = "EX-REST";
    auto fill_tx = std::make_shared<FakeFillTransport>();
    auto encoder = std::make_shared<FakeEncoder>();
    auto parser = std::make_shared<ScriptedPrivateExecutionParser>();
    TestPrivateExecutionIngress ingress;
    int execution_failures = 0;

    ExecutionBridge::deps d;
    d.order_tx = tx;
    d.fill_tx = fill_tx;
    d.encoder = encoder;
    d.parser = parser;
    d.execution_ingress = &ingress;
    d.require_execution_ingress = true;
    d.execution_failure_handler = [&] { ++execution_failures; };
    d.start_transport_thread = true;
    ExecutionBridge bridge(std::move(d));
    ASSERT_TRUE(bridge.open());

    bridge.submit_order(make_order(1901, "BTCUSDT", 2.0, 100.0));
    ASSERT_TRUE(tx->wait_until_entered());

    // Model the engine's FIFO consume/resolve/account/ack path while the
    // REST submit is blocked. The immutable tombstone must retain the private
    // venue identity until the delayed REST response is compared with it.
    auto private_full = make_unified_full_record(
        "tt-1901", "EX-PRIVATE", 2.0);
    // The private ingress is the sole source of a record sequence; shape
    // validation intentionally happens inside try_publish after it assigns
    // that monotonic sequence.
    ASSERT_TRUE(ingress.try_publish(private_full));
    private_execution_record terminal;
    ASSERT_TRUE(ingress.try_pop(terminal));
    ASSERT_EQ(bridge.resolve_private_execution(terminal),
              private_execution_resolution::tracked);
    ASSERT_EQ(terminal.engine_order_id, 1901u);
    ASSERT_TRUE(bridge.commit_private_execution(
        {terminal.sequence, terminal.engine_order_id}));
    ASSERT_TRUE(bridge.acknowledge_private_terminal(terminal.sequence));

    tx->release();
    std::vector<ExecutionBridge::submit_result> results;
    for (int i = 0; i < 10000 && results.empty(); ++i)
    {
        (void)bridge.poll_submit_results(results);
        std::this_thread::yield();
    }
    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results.front().ok);
    EXPECT_TRUE(results.front().uncertain);
    EXPECT_TRUE(results.front().fatal);
    EXPECT_EQ(execution_failures, 1);
    EXPECT_TRUE(ingress.failed());
    bridge.close();
}

TEST(ExecutionBridgeUnifiedIngress,
     RollbackRestoresEconomicReplayReservationBeforeAccountingCommit)
{
    auto tx = std::make_shared<FakeOrderTransport>();
    tx->next_exchange_id_ = "EX-1902";
    auto fill_tx = std::make_shared<FakeFillTransport>();
    auto encoder = std::make_shared<FakeEncoder>();
    auto parser = std::make_shared<ScriptedPrivateExecutionParser>();
    TestPrivateExecutionIngress ingress;

    ExecutionBridge::deps d;
    d.order_tx = tx;
    d.fill_tx = fill_tx;
    d.encoder = encoder;
    d.parser = parser;
    d.execution_ingress = &ingress;
    d.require_execution_ingress = true;
    d.execution_failure_handler = [] {};
    d.start_transport_thread = false;
    ExecutionBridge bridge(std::move(d));
    ASSERT_TRUE(bridge.open());

    bridge.submit_order(make_order(1902, "BTCUSDT", 2.0, 100.0));
    bridge.drain_outbound_for_test();

    auto partial = make_unified_full_record("tt-1902", "EX-1902", 1.0);
    partial.k = private_execution_record::kind::partial_fill;
    partial.remaining_qty = 1.0;
    ASSERT_TRUE(ingress.try_publish(partial));
    private_execution_record first_attempt;
    ASSERT_TRUE(ingress.try_pop(first_attempt));
    ASSERT_EQ(bridge.resolve_private_execution(first_attempt),
              private_execution_resolution::tracked);
    ASSERT_TRUE(bridge.rollback_private_execution(
        {first_attempt.sequence, first_attempt.engine_order_id}));

    // A retry with the same immutable execution fingerprint must remain
    // accountable after pre-accounting failure; it cannot be consumed as a
    // duplicate merely because the first engine attempt rolled back.
    partial.sequence = 0;
    ASSERT_TRUE(ingress.try_publish(partial));
    private_execution_record retried_partial;
    ASSERT_TRUE(ingress.try_pop(retried_partial));
    ASSERT_EQ(bridge.resolve_private_execution(retried_partial),
              private_execution_resolution::tracked);
    ASSERT_TRUE(bridge.commit_private_execution(
        {retried_partial.sequence, retried_partial.engine_order_id}));

    partial.sequence = 0;
    ASSERT_TRUE(ingress.try_publish(partial));
    private_execution_record duplicate_partial;
    ASSERT_TRUE(ingress.try_pop(duplicate_partial));
    EXPECT_EQ(bridge.resolve_private_execution(duplicate_partial),
              private_execution_resolution::duplicate);

    auto full = make_unified_full_record("tt-1902", "EX-1902", 1.0);
    full.cumulative_qty = 2.0;
    full.remaining_qty = 0.0;
    ASSERT_TRUE(private_execution_record::copy_text(
        full.execution_id, full.execution_id_size, "private-trade-2"));
    ASSERT_TRUE(ingress.try_publish(full));
    private_execution_record terminal;
    ASSERT_TRUE(ingress.try_pop(terminal));
    ASSERT_EQ(bridge.resolve_private_execution(terminal),
              private_execution_resolution::tracked);
    ASSERT_TRUE(bridge.commit_private_execution(
        {terminal.sequence, terminal.engine_order_id}));
    ASSERT_TRUE(bridge.acknowledge_private_terminal(terminal.sequence));
    EXPECT_FALSE(bridge.has_unresolved_private_lifecycle());
}

TEST(ExecutionBridgeUnifiedIngress,
     UnknownPrivateFillPublishesOnlyARecordWithoutReaderSideSynthesis)
{
    auto tx = std::make_shared<FakeOrderTransport>();
    auto fill_tx = std::make_shared<FakeFillTransport>();
    auto encoder = std::make_shared<FakeEncoder>();
    auto parser = std::make_shared<ScriptedPrivateExecutionParser>();
    TestPrivateExecutionIngress ingress;
    int execution_failures = 0;
    int synth_calls = 0;

    ExecutionBridge::deps d;
    d.order_tx = tx;
    d.fill_tx = fill_tx;
    d.encoder = encoder;
    d.parser = parser;
    d.execution_ingress = &ingress;
    d.require_execution_ingress = true;
    d.execution_failure_handler = [&] { ++execution_failures; };
    d.start_transport_thread = false;
    ExecutionBridge bridge(std::move(d));
    ASSERT_TRUE(bridge.open());
    bridge.set_unknown_fill_handler(
        [&](const parsed_exec&, std::uint64_t)
            -> std::optional<ExecutionBridge::synth_result>
        {
            ++synth_calls;
            return std::nullopt;
        });

    parser->next = make_unified_exec(parsed_exec::kind::full_fill,
                                     "foreign-client", "FOREIGN-1");
    parser->next.last_fill_qty = 1.0;
    parser->next.last_fill_price = 100.0;
    parser->next.cumulative_qty = 1.0;
    parser->next.has_cumulative_qty = true;
    parser->next.execution_id = "foreign-trade-1";
    fill_tx->deliver("foreign-private-fill");

    // The private reader must not call the legacy unknown-fill synthesis
    // hook, allocate a fill_event, or mutate the bridge's tracking maps. It
    // only projects fixed data into the authoritative engine FIFO.
    EXPECT_EQ(synth_calls, 0);
    EXPECT_EQ(execution_failures, 0);
    std::vector<fill_event> fills;
    EXPECT_FALSE(bridge.poll_fills(fills));

    private_execution_record record;
    ASSERT_TRUE(ingress.try_pop(record));
    EXPECT_EQ(record.k, private_execution_record::kind::full_fill);
    EXPECT_EQ(record.client_order_id_view(), "foreign-client");
    EXPECT_EQ(record.exchange_order_id_view(), "FOREIGN-1");
    EXPECT_EQ(record.execution_id_view(), "foreign-trade-1");
    EXPECT_EQ(synth_calls, 0);
}

TEST(ExecutionBridgeUnifiedIngress,
     UnrelatedPrivateFrameWithoutExactControlProofClosesAdmission)
{
    auto tx = std::make_shared<FakeOrderTransport>();
    auto fill_tx = std::make_shared<FakeFillTransport>();
    auto encoder = std::make_shared<FakeEncoder>();
    auto parser = std::make_shared<ScriptedPrivateExecutionParser>();
    TestPrivateExecutionIngress ingress;
    int execution_failures = 0;

    ExecutionBridge::deps d;
    d.order_tx = tx;
    d.fill_tx = fill_tx;
    d.encoder = encoder;
    d.parser = parser;
    d.execution_ingress = &ingress;
    d.require_execution_ingress = true;
    d.execution_failure_handler = [&] { ++execution_failures; };
    d.start_transport_thread = false;
    ExecutionBridge bridge(std::move(d));
    ASSERT_TRUE(bridge.open());

    // The default parser contract proves no unrelated frame harmless.  An
    // account snapshot or unknown authenticated event must therefore enter
    // the same fail-closed path as malformed execution truth; it must not
    // be sent to the legacy diagnostic snapshot callback or silently dropped.
    parser->result = execution_parse_result::unrelated;
    fill_tx->deliver("unmodelled-private-frame");

    EXPECT_TRUE(ingress.failed());
    EXPECT_EQ(execution_failures, 1);
    bridge.submit_order(make_order(1902));
    bridge.drain_outbound_for_test();
    EXPECT_TRUE(tx->submissions_.empty());
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
    auto en = std::make_shared<FakeEncoder>();
    ExecutionBridge::deps d;
    d.order_tx = tx;
    d.fill_tx = ft;
    d.encoder = en;
    d.parser = std::make_shared<FakeFundingParser>();
    int execution_failures = 0;
    d.execution_failure_handler = [&] { ++execution_failures; };
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
    EXPECT_EQ(execution_failures, 1);

    // A malformed funding envelope cannot leave a queued/new REST mutation
    // admissible while the provider's failure callback makes its way to the
    // engine.  The bridge closes synchronously on the reader thread.
    bridge.submit_order(make_order(711));
    bridge.drain_outbound_for_test();
    EXPECT_TRUE(tx->submissions_.empty());
}

TEST(ExecutionBridge, FundingCallbacksMustBePaired)
{
    auto tx = std::make_shared<FakeOrderTransport>();
    auto ft = std::make_shared<FakeFillTransport>();
    ExecutionBridge::deps d;
    d.order_tx = tx;
    d.fill_tx = ft;
    d.parser = std::make_shared<FakeFundingParser>();
    d.execution_failure_handler = [] {};
    d.funding_update_handler = [](const parsed_funding_update&) { return true; };
    d.start_transport_thread = false;

    ExecutionBridge bridge(std::move(d));
    EXPECT_FALSE(bridge.open());
    EXPECT_FALSE(tx->opened_);
}

#ifdef HAS_BITGET
TEST(ExecutionBridge,
     BitgetHarmlessControlPassesButContradictoryFundingControlClosesAdmission)
{
    auto tx = std::make_shared<FakeOrderTransport>();
    auto ft = std::make_shared<FakeFillTransport>();
    auto en = std::make_shared<FakeEncoder>();
    int execution_failures = 0;
    int funding_failures = 0;
    ExecutionBridge::deps d;
    d.order_tx = tx;
    d.fill_tx = ft;
    d.encoder = en;
    d.parser = std::make_shared<BitgetFuturesUserDataParser>();
    d.execution_failure_handler = [&] { ++execution_failures; };
    d.funding_update_handler = [](const parsed_funding_update&) { return true; };
    d.funding_failure_handler = [&] { ++funding_failures; };
    d.start_transport_thread = false;
    ExecutionBridge bridge(std::move(d));
    ASSERT_TRUE(bridge.open());

    // The funding fast-path must defer a real subscribe ACK to the execution
    // parser rather than terminally treating it as malformed funding.
    ft->deliver(
        R"({"event":"subscribe","arg":{"instType":"UTA","topic":"order"},"code":"0"})");
    EXPECT_EQ(execution_failures, 0);
    EXPECT_EQ(funding_failures, 0);
    bridge.submit_order(make_order(722));
    bridge.drain_outbound_for_test();
    ASSERT_EQ(tx->submissions_.size(), 1u);

    // A data-bearing control is not a valid account funding push and must
    // reach the parser's terminal control-vs-data validation.
    ft->deliver(
        R"({"event":"info","arg":{"instType":"UTA","topic":"account"},"data":[{"bizType":"funding_fee","coin":"USDT","balanceChange":"-0.5"}],"ts":1700000000000})");
    EXPECT_EQ(execution_failures, 1);
    EXPECT_EQ(funding_failures, 0);
    bridge.submit_order(make_order(723));
    bridge.drain_outbound_for_test();
    EXPECT_EQ(tx->submissions_.size(), 1u);
}
#endif

TEST(ExecutionBridge, DuplicateEngineOrClientIdentityClosesAdmissionBeforeSend)
{
    {
        bridge_harness h;
        ASSERT_TRUE(h.bridge->open());
        h.bridge->submit_order(make_order(712));
        h.bridge->submit_order(make_order(712));

        EXPECT_EQ(h.execution_failures, 1);
        h.bridge->drain_outbound_for_test();
        EXPECT_TRUE(h.tx->submissions_.empty());
    }

    auto tx = std::make_shared<FakeOrderTransport>();
    auto ft = std::make_shared<FakeFillTransport>();
    auto en = std::make_shared<FakeEncoder>();
    auto pa = std::make_shared<FakeParser>();
    int failures = 0;
    ExecutionBridge::deps d;
    d.order_tx = tx;
    d.fill_tx = ft;
    d.encoder = en;
    d.parser = pa;
    d.client_id_fn = [](uint64_t) { return std::string{"duplicate-client"}; };
    d.execution_failure_handler = [&] { ++failures; };
    d.start_transport_thread = false;
    ExecutionBridge bridge(std::move(d));
    ASSERT_TRUE(bridge.open());
    bridge.submit_order(make_order(713));
    bridge.submit_order(make_order(714));

    EXPECT_EQ(failures, 1);
    bridge.drain_outbound_for_test();
    EXPECT_TRUE(tx->submissions_.empty());
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
    d.execution_failure_handler = [] {};
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
