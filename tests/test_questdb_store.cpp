#ifdef HAS_QUESTDB

#include "data/questdb/store.h"
#include "core/event.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

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

TEST(QuestdbStore, BeginIssues7DdlsInOrder)
{
    Fixture f;
    ASSERT_TRUE(f.store->begin());
    ASSERT_EQ(f.ddls_seen.size(), 7u);
    EXPECT_TRUE(contains(f.ddls_seen[0], "runs_meta"));
    EXPECT_TRUE(contains(f.ddls_seen[1], "tt_test_orders"));
    EXPECT_TRUE(contains(f.ddls_seen[2], "tt_test_order_status"));
    EXPECT_TRUE(contains(f.ddls_seen[3], "tt_test_fills"));
    EXPECT_TRUE(contains(f.ddls_seen[4], "tt_test_rejections"));
    EXPECT_TRUE(contains(f.ddls_seen[5], "tt_test_cancellations"));
    EXPECT_TRUE(contains(f.ddls_seen[6], "tt_test_amendments"));
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

#endif // HAS_QUESTDB
