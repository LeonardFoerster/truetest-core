#ifdef HAS_QUESTDB

#include "data/questdb/store.h"
#include "data/questdb/tcp_client.h"
#include "core/event.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <future>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using truetest::questdb::IIlpTransport;
using truetest::questdb::IlpWriter;
using truetest::questdb::QuestdbStore;
using truetest::questdb::StoreConfig;

namespace {

class RecordingTransport : public IIlpTransport
{
public:
    bool connect(const std::string&, std::uint16_t) override
    {
        connected_ = true;
        return true;
    }
    bool write_all(std::string_view data) override
    {
        if (fail_writes) return false;
        // Each write may contain >1 line - split on '\n'.
        std::string buf;
        for (char c : data)
        {
            if (c == '\n')
            {
                lines.emplace_back(std::move(buf));
                buf.clear();
            }
            else
            {
                buf.push_back(c);
            }
        }
        if (!buf.empty()) lines.emplace_back(std::move(buf));
        return true;
    }
    void close() override { connected_ = false; }
    bool is_connected() const override { return connected_; }

    std::vector<std::string> lines;
    bool fail_writes = false;

private:
    bool connected_ = false;
};

struct Fixture
{
    StoreConfig cfg{};
    RecordingTransport* transport = nullptr;
    std::vector<std::string> ddls_seen;
    std::unique_ptr<QuestdbStore> store;

    Fixture()
    {
        cfg.run_tag = "tt_test";
        cfg.mode = "backtest";
        cfg.binary = "engine_backtest";
        cfg.strategy = "sma";
        cfg.symbol = "BTCUSDT";
        cfg.initial_equity = 10000.0;

        auto t = std::make_unique<RecordingTransport>();
        transport = t.get();
        // Use flush-every-line=1 so we see each enqueue immediately in the
        // recorded transport, instead of having to flush manually.
        auto writer = std::make_unique<IlpWriter>(
            cfg.host, cfg.ilp_port, std::move(t),
            /*flush_every_n_lines=*/1);
        store = std::make_unique<QuestdbStore>(
            cfg, std::move(writer),
            [this](const std::string& sql) {
                ddls_seen.push_back(sql);
                return true;
            });
    }
};

bool starts_with(const std::string& s, const std::string& prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool contains(const std::string& s, const std::string& needle)
{
    return s.find(needle) != std::string::npos;
}

std::shared_ptr<order_event> make_order(std::uint64_t id = 42)
{
    auto o = std::make_shared<order_event>(
        std::chrono::system_clock::now(),
        "BTCUSDT", order_type::limit, order_side::buy,
        0.001, 50000.0, time_in_force::gtc);
    o->set_order_id(id);
    o->set_strategy_name("sma");
    return o;
}

}

TEST(QuestdbStore, BeginIssuesDdlsInOrder)
{
    Fixture f;
    ASSERT_TRUE(f.store->begin());
    ASSERT_EQ(f.ddls_seen.size(), 9u);
    EXPECT_TRUE(contains(f.ddls_seen[0], "runs_meta"));
    EXPECT_TRUE(contains(f.ddls_seen[1], "tt_test_orders"));
    EXPECT_TRUE(contains(f.ddls_seen[2], "tt_test_order_status"));
    EXPECT_TRUE(contains(f.ddls_seen[3], "tt_test_fills"));
    EXPECT_TRUE(contains(f.ddls_seen[4], "tt_test_funding"));
    EXPECT_TRUE(contains(f.ddls_seen[5], "tt_test_events"));
    EXPECT_TRUE(contains(f.ddls_seen[6], "tt_test_rejections"));
    EXPECT_TRUE(contains(f.ddls_seen[7], "tt_test_cancellations"));
    EXPECT_TRUE(contains(f.ddls_seen[8], "tt_test_amendments"));
}

TEST(QuestdbStore, StrictRuntimeWriteFailureLatches)
{
    StoreConfig cfg{};
    cfg.run_tag = "tt_strict_failure";
    cfg.strict = true;
    auto transport = std::make_unique<RecordingTransport>();
    auto* raw = transport.get();
    auto writer = std::make_unique<IlpWriter>(
        cfg.host, cfg.ilp_port, std::move(transport), 1);
    QuestdbStore store(cfg, std::move(writer),
                       [](const std::string&) { return true; });
    ASSERT_TRUE(store.begin());
    raw->fail_writes = true;
    auto order = make_order();
    store.record_order_submitted(*order, "pending");
    EXPECT_TRUE(store.strict_failure_latched());
}

TEST(QuestdbStore, BeginFailsWhenInitialMetadataCannotBeBuffered)
{
    StoreConfig cfg{};
    cfg.run_tag = "tt_zero_pending_cap";
    cfg.max_pending_bytes = 0;
    auto transport = std::make_unique<RecordingTransport>();
    auto* raw = transport.get();
    auto writer = std::make_unique<IlpWriter>(
        cfg.host, cfg.ilp_port, std::move(transport),
        /*flush_every_n_lines=*/1000,
        std::chrono::milliseconds{50},
        cfg.max_pending_bytes);
    QuestdbStore store(cfg, std::move(writer),
                       [](const std::string&) { return true; });

    EXPECT_FALSE(store.begin());
    EXPECT_TRUE(raw->lines.empty());
}

TEST(QuestdbStore, BeginRejectsInvalidRunTagBeforeDdl)
{
    Fixture f;
    f.cfg.run_tag = "bad tag;DROP";

    auto t = std::make_unique<RecordingTransport>();
    auto* tp = t.get();
    auto writer = std::make_unique<IlpWriter>(
        f.cfg.host, f.cfg.ilp_port, std::move(t),
        /*flush_every_n_lines=*/1);
    QuestdbStore store(f.cfg, std::move(writer),
        [](const std::string&) {
            ADD_FAILURE() << "DDL should not run for invalid run_tag";
            return true;
        });

    EXPECT_FALSE(store.begin());
    EXPECT_TRUE(tp->lines.empty());
}

TEST(QuestdbStore, BeginAbortsIfAnyDdlFails)
{
    StoreConfig cfg;
    cfg.run_tag = "tt";
    auto t = std::make_unique<RecordingTransport>();
    auto* tp = t.get();
    auto writer = std::make_unique<IlpWriter>(cfg.host, cfg.ilp_port,
                                              std::move(t), 1);
    int call = 0;
    QuestdbStore store(cfg, std::move(writer),
        [&call](const std::string&) {
            return ++call != 4; // fail the 4th DDL
        });
    EXPECT_FALSE(store.begin());
    EXPECT_EQ(tp->lines.size(), 0u); // no ILP rows enqueued
}

TEST(QuestdbStore, BeginWritesRunsMetaRow)
{
    Fixture f;
    ASSERT_TRUE(f.store->begin());
    ASSERT_GE(f.transport->lines.size(), 1u);
    EXPECT_TRUE(starts_with(f.transport->lines[0], "runs_meta,"));
    EXPECT_TRUE(contains(f.transport->lines[0], "run_tag=tt_test"));
    EXPECT_TRUE(contains(f.transport->lines[0], "initial_equity=10000"));
}

TEST(QuestdbStore, RecordOrderSubmittedProducesOrdersRow)
{
    Fixture f;
    ASSERT_TRUE(f.store->begin());
    f.transport->lines.clear();

    auto o = make_order(99);
    f.store->record_order_submitted(*o, "pending");

    ASSERT_EQ(f.transport->lines.size(), 1u);
    const auto& line = f.transport->lines[0];
    EXPECT_TRUE(starts_with(line, "tt_test_orders,"));
    EXPECT_TRUE(contains(line, "side=buy"));
    EXPECT_TRUE(contains(line, "type=lmt"));
    EXPECT_TRUE(contains(line, "initial_status=pending"));
    EXPECT_TRUE(contains(line, "order_id=99i"));
}

TEST(QuestdbStore, RecordStatusTransitionProducesOrderStatusRow)
{
    Fixture f;
    ASSERT_TRUE(f.store->begin());
    f.transport->lines.clear();

    f.store->record_status_transition(7, order_status::pending,
                                      order_status::open, "accepted");
    ASSERT_EQ(f.transport->lines.size(), 1u);
    const auto& line = f.transport->lines[0];
    EXPECT_TRUE(starts_with(line, "tt_test_order_status,"));
    EXPECT_TRUE(contains(line, "old_status=pending"));
    EXPECT_TRUE(contains(line, "new_status=open"));
    EXPECT_TRUE(contains(line, "order_id=7i"));
}

TEST(QuestdbStore, RecordFillProducesFillsRowWithOpener)
{
    Fixture f;
    ASSERT_TRUE(f.store->begin());
    f.transport->lines.clear();

    fill_event fl(std::chrono::system_clock::now(), "BTCUSDT",
                  /*order_id=*/77, order_side::sell,
                  0.5, 51000.0, /*commission=*/0.1,
                  /*remaining_qty=*/0.0, /*fill_id=*/123);
    f.store->record_fill(fl, /*opener=*/55, "sma", "exchange");

    ASSERT_EQ(f.transport->lines.size(), 1u);
    const auto& line = f.transport->lines[0];
    EXPECT_TRUE(starts_with(line, "tt_test_fills,"));
    EXPECT_TRUE(contains(line, "order_id=77i"));
    EXPECT_TRUE(contains(line, "opener_order_id=55i"));
    EXPECT_TRUE(contains(line, "fill_id=123i"));
    EXPECT_TRUE(contains(line, "side=sell"));
    EXPECT_TRUE(contains(line, "source=exchange"));
}

TEST(QuestdbStore, RecordRejectionProducesRejectionsRowWithReason)
{
    Fixture f;
    ASSERT_TRUE(f.store->begin());
    f.transport->lines.clear();

    auto o = make_order(11);
    f.store->record_rejection(*o, "risk_halt", "max_position_exceeded");
    ASSERT_EQ(f.transport->lines.size(), 1u);
    const auto& line = f.transport->lines[0];
    EXPECT_TRUE(starts_with(line, "tt_test_rejections,"));
    EXPECT_TRUE(contains(line, "reason=risk_halt"));
    EXPECT_TRUE(contains(line, "reason_detail=\"max_position_exceeded\""));
    EXPECT_TRUE(contains(line, "order_id=11i"));
}

TEST(QuestdbStore, RecordCancellationProducesCancellationsRow)
{
    Fixture f;
    ASSERT_TRUE(f.store->begin());
    f.transport->lines.clear();

    f.store->record_cancellation(33, "BTCUSDT", "sma", "manual");
    ASSERT_EQ(f.transport->lines.size(), 1u);
    const auto& line = f.transport->lines[0];
    EXPECT_TRUE(starts_with(line, "tt_test_cancellations,"));
    EXPECT_TRUE(contains(line, "order_id=33i"));
    EXPECT_TRUE(contains(line, "reason=manual"));
}

TEST(QuestdbStore, RecordAmendmentProducesAmendmentsRow)
{
    Fixture f;
    ASSERT_TRUE(f.store->begin());
    f.transport->lines.clear();

    f.store->record_amendment(44, "BTCUSDT", 50000.0, 50500.0, 0.1, 0.2,
                              std::chrono::system_clock::now());
    ASSERT_EQ(f.transport->lines.size(), 1u);
    const auto& line = f.transport->lines[0];
    EXPECT_TRUE(starts_with(line, "tt_test_amendments,"));
    EXPECT_TRUE(contains(line, "old_price=50000"));
    EXPECT_TRUE(contains(line, "new_price=50500"));
    EXPECT_TRUE(contains(line, "order_id=44i"));
}

TEST(QuestdbStore, EndWritesSecondRunsMetaRow)
{
    Fixture f;
    ASSERT_TRUE(f.store->begin());
    f.transport->lines.clear();

    f.store->end(11000.0, /*orders=*/5, /*fills=*/4, /*rejs=*/1);
    ASSERT_EQ(f.transport->lines.size(), 1u);
    const auto& line = f.transport->lines[0];
    EXPECT_TRUE(starts_with(line, "runs_meta,"));
    EXPECT_TRUE(contains(line, "ended_at="));
    EXPECT_TRUE(contains(line, "final_equity=11000"));
    EXPECT_TRUE(contains(line, "total_orders=5i"));
}

TEST(QuestdbStore, AllRowsTaggedWithRunTag)
{
    Fixture f;
    ASSERT_TRUE(f.store->begin());

    auto o = make_order(1);
    f.store->record_order_submitted(*o, "pending");
    f.store->record_status_transition(1, order_status::pending,
                                      order_status::open);
    fill_event fl(std::chrono::system_clock::now(), "BTCUSDT", 1,
                  order_side::buy, 0.001, 50000.0);
    f.store->record_fill(fl, 0, "sma", "simulated");
    f.store->record_cancellation(2, "BTCUSDT", "sma", "user");
    f.store->record_amendment(3, "BTCUSDT", 1, 2, 3, 4,
                              std::chrono::system_clock::now());
    f.store->record_rejection(*o, "risk", "limit");
    f.store->end(10000, 1, 1, 1);

    ASSERT_GE(f.transport->lines.size(), 7u);
    for (const auto& line : f.transport->lines)
    {
        EXPECT_TRUE(contains(line, "run_tag=tt_test"))
            << "missing run_tag in: " << line;
    }
}

TEST(QuestdbStore, FlushIsCallableWithoutCrash)
{
    Fixture f;
    ASSERT_TRUE(f.store->begin());
    // No-op (buffer empty after begin's flush) - must not crash.
    f.store->flush();
    f.store->tick();
    SUCCEED();
}

TEST(QuestdbStore, RecordFundingProducesFundingRow)
{
    Fixture f;
    ASSERT_TRUE(f.store->begin());
    f.transport->lines.clear();

    funding_event fe(std::chrono::system_clock::now(),
                     "BTCUSDT",
                     /*qty_change=*/0.0,
                     /*cash_delta=*/12.345,
                     "FUNDING_FEE");

    f.store->record_funding(fe, f.cfg.run_tag);

    ASSERT_EQ(f.transport->lines.size(), 1u);
    const auto& line = f.transport->lines[0];
    EXPECT_TRUE(starts_with(line, "tt_test_funding,"));
    EXPECT_TRUE(contains(line, "symbol=BTCUSDT"));
    EXPECT_TRUE(contains(line, "reason=FUNDING_FEE"));
    EXPECT_TRUE(contains(line, "cash_delta=12.345"));
    EXPECT_TRUE(contains(line, "qty_change=0"));
}

TEST(QuestdbStore, RecordEventProducesEventsRow)
{
    Fixture f;
    ASSERT_TRUE(f.store->begin());
    f.transport->lines.clear();

    f.store->record_event(
        "risk_decision",
        "BTCUSDT",
        "sma",
        12345,
        "reject",
        "position limit exceeded",
        R"({"limit": 10, "current": 12})"
    );

    ASSERT_EQ(f.transport->lines.size(), 1u);
    const auto& line = f.transport->lines[0];
    EXPECT_TRUE(starts_with(line, "tt_test_events,"));
    EXPECT_TRUE(contains(line, "event_type=risk_decision"));
    EXPECT_TRUE(contains(line, "symbol=BTCUSDT"));
    EXPECT_TRUE(contains(line, "order_id=12345i"));
    EXPECT_TRUE(contains(line, "severity=reject"));
    EXPECT_TRUE(contains(line, "message=\"position limit exceeded\""));
    EXPECT_TRUE(contains(line, "details=\"{\\\"limit\\\": 10, \\\"current\\\": 12}\""));
}

TEST(QuestdbStore, TimeBasedFlushFiresViaTick)
{
    // This test verifies the Phase 1 time-based flushing path:
    // When line count threshold is high, tick() should still cause a flush
    // once the time threshold is reached.

    StoreConfig cfg{};
    cfg.run_tag = "tt_time_flush";
    cfg.mode = "shadow";
    cfg.binary = "engine_shadow";
    cfg.strategy = "test";
    cfg.symbol = "ETHUSDT";
    cfg.initial_equity = 50000.0;

    auto transport = std::make_unique<RecordingTransport>();
    RecordingTransport* raw_transport = transport.get();

    // High line threshold (1000), short time threshold (5ms)
    auto writer = std::make_unique<IlpWriter>(
        cfg.host, cfg.ilp_port, std::move(transport),
        /*flush_every_n_lines=*/1000,
        /*flush_every=*/std::chrono::milliseconds(5));

    std::vector<std::string> ddls;
    auto store = std::make_unique<QuestdbStore>(
        cfg,
        std::move(writer),
        [&ddls](const std::string& sql) {
            ddls.push_back(sql);
            return true;
        });

    ASSERT_TRUE(store->begin());
    raw_transport->lines.clear();  // clear the initial runs_meta row from begin

    // Enqueue one record — far below the 1000 line threshold
    auto o = make_order(999);
    store->record_order_submitted(*o, "pending");

    // Should still be zero because we haven't hit line count and haven't called tick() yet
    EXPECT_EQ(raw_transport->lines.size(), 0u);

    // Give the clock enough time past the 5ms threshold and call tick().
    // This exercises the time-based flush path (Phase 1) even when far below
    // the line count threshold.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    store->tick();

    // Now we expect the enqueued line to have been flushed via the time path
    EXPECT_GE(raw_transport->lines.size(), 1u);
    if (!raw_transport->lines.empty())
    {
        const auto& line = raw_transport->lines.back();
        EXPECT_TRUE(starts_with(line, "tt_time_flush_orders,"));
        EXPECT_TRUE(contains(line, "order_id=999i"));
    }
}

TEST(QuestdbTcpClient, PartialWriteToBlockedPeerHonorsTotalDeadline)
{
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listener, 0);
    const int small_buffer = 4096;
    ASSERT_EQ(::setsockopt(listener, SOL_SOCKET, SO_RCVBUF,
                          &small_buffer, sizeof(small_buffer)), 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ASSERT_EQ(::bind(listener, reinterpret_cast<sockaddr*>(&addr),
                     sizeof(addr)), 0);
    ASSERT_EQ(::listen(listener, 1), 0);
    socklen_t addr_len = sizeof(addr);
    ASSERT_EQ(::getsockname(listener, reinterpret_cast<sockaddr*>(&addr),
                            &addr_len), 0);

    std::promise<void> accepted;
    auto accepted_future = accepted.get_future();
    std::promise<void> release;
    auto release_future = release.get_future();
    std::atomic<std::size_t> received{0};
    std::thread peer([&] {
        pollfd listener_poll{listener, POLLIN, 0};
        if (::poll(&listener_poll, 1, 1000) <= 0) return;
        const int fd = ::accept(listener, nullptr, nullptr);
        if (fd < 0) return;
        accepted.set_value();
        release_future.wait();
        char buffer[8192];
        for (;;)
        {
            const auto n = ::recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0) break;
            received.fetch_add(static_cast<std::size_t>(n),
                               std::memory_order_relaxed);
        }
        ::close(fd);
    });

    truetest::questdb::TcpClient client;
    if (!client.connect("127.0.0.1", ntohs(addr.sin_port), 500))
    {
        release.set_value();
        ::shutdown(listener, SHUT_RDWR);
        ::close(listener);
        peer.join();
        FAIL() << "loopback client failed to connect";
        return;
    }
    if (accepted_future.wait_for(std::chrono::seconds(1))
        != std::future_status::ready)
    {
        client.close();
        release.set_value();
        ::shutdown(listener, SHUT_RDWR);
        ::close(listener);
        peer.join();
        FAIL() << "loopback peer did not accept the client";
        return;
    }
    const std::string payload(8 * 1024 * 1024, 'x');
    const auto start = std::chrono::steady_clock::now();
    const auto write = client.write_attempt(payload, 50);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    client.close();
    release.set_value();
    peer.join();
    ::close(listener);

    EXPECT_LT(elapsed, std::chrono::milliseconds(500));
    EXPECT_EQ(write.state,
              truetest::questdb::TcpClient::WriteState::delivery_ambiguous);
    EXPECT_GT(write.bytes_sent, 0u);
    EXPECT_LT(write.bytes_sent, payload.size());
    EXPECT_GT(received.load(std::memory_order_relaxed), 0u);
    EXPECT_LT(received.load(std::memory_order_relaxed), payload.size());
}

#endif // HAS_QUESTDB
