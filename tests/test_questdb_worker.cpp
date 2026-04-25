#ifdef HAS_QUESTDB

#include "data/questdb/ilp_writer.h"
#include "data/questdb/store.h"
#include "engine/questdb_worker.h"
#include "threading/ring_buffer.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

using truetest::questdb::IIlpTransport;
using truetest::questdb::IlpWriter;
using truetest::questdb::QuestdbStore;
using truetest::questdb::StoreConfig;

namespace {

class MockStore : public QuestdbStore
{
public:
    MockStore()
        : QuestdbStore(make_cfg(),
                       std::make_unique<IlpWriter>("h", 9009,
                           std::unique_ptr<IIlpTransport>(nullptr), 1),
                       [](const std::string&) { return true; })
    {}

    void record_order_submitted(const order_event&,
                                const std::string&) override { ++orders; }
    void record_status_transition(std::uint64_t, order_status,
                                  order_status,
                                  const std::string&) override { ++status; }
    void record_fill(const fill_event&, std::uint64_t,
                     const std::string&,
                     const std::string&) override { ++fills; }
    void record_rejection(const order_event&,
                          const std::string&,
                          const std::string&) override { ++rejections; }
    void record_cancellation(std::uint64_t,
                             const std::string&,
                             const std::string&,
                             const std::string&) override { ++cancellations; }
    void record_amendment(std::uint64_t,
                          const std::string&,
                          double, double, double, double,
                          std::chrono::system_clock::time_point) override
    { ++amendments; }
    void tick() override { ++ticks; }
    void flush() override { ++flushes; }

    int orders = 0, status = 0, fills = 0, rejections = 0;
    int cancellations = 0, amendments = 0;
    int ticks = 0, flushes = 0;

private:
    static StoreConfig make_cfg()
    {
        StoreConfig c;
        c.run_tag = "mock";
        return c;
    }
};

using event_pointer = std::shared_ptr<event>;
using EventRing = RingBuffer<event_pointer, 65536>;

} // namespace

TEST(QuestdbWorker, DrainsRingUntilStopped)
{
    auto store = std::make_shared<MockStore>();
    QuestDbWorker worker(store);
    EventRing ring;

    for (int i = 0; i < 100; ++i)
    {
        auto o = std::make_shared<order_event>(
            std::chrono::system_clock::now(),
            "BTC", order_type::limit, order_side::buy, 1.0, 100.0);
        o->set_order_id(static_cast<std::uint64_t>(i + 1));
        ASSERT_TRUE(ring.try_push(o));
    }

    std::thread th([&] { worker.run(ring); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    worker.stop();
    th.join();

    EXPECT_EQ(store->orders, 100);
}

TEST(QuestdbWorker, DispatchesByEventType)
{
    auto store = std::make_shared<MockStore>();
    QuestDbWorker worker(store);
    EventRing ring;

    auto now = std::chrono::system_clock::now();

    auto o = std::make_shared<order_event>(
        now, "BTC", order_type::limit, order_side::buy, 1.0, 100.0);
    o->set_order_id(1);
    auto f = std::make_shared<fill_event>(now, "BTC", 1, order_side::buy, 0.5, 100.0);
    auto r = std::make_shared<rejection_event>(now, "BTC", 2, "bad");
    auto c = std::make_shared<cancel_event>(now, "BTC", 3, "user");
    auto a = std::make_shared<amend_event>(now, "BTC", 4, 101.0, 0.5);

    ASSERT_TRUE(ring.try_push(o));
    ASSERT_TRUE(ring.try_push(f));
    ASSERT_TRUE(ring.try_push(r));
    ASSERT_TRUE(ring.try_push(c));
    ASSERT_TRUE(ring.try_push(a));

    std::thread th([&] { worker.run(ring); });
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    worker.stop();
    th.join();

    EXPECT_EQ(store->orders, 1);
    EXPECT_EQ(store->fills, 1);
    EXPECT_EQ(store->rejections, 1);
    EXPECT_EQ(store->cancellations, 1);
    EXPECT_EQ(store->amendments, 1);
}

TEST(QuestdbWorker, IgnoresUncapturedEventTypes)
{
    auto store = std::make_shared<MockStore>();
    QuestDbWorker worker(store);
    EventRing ring;

    auto now = std::chrono::system_clock::now();
    auto m = std::make_shared<market_event>(now, "BTC", 1, 2, 0.5, 1.5, 100);
    auto t = std::make_shared<tick_event>(now, "BTC", 1.0, 1);

    ASSERT_TRUE(ring.try_push(m));
    ASSERT_TRUE(ring.try_push(t));

    std::thread th([&] { worker.run(ring); });
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    worker.stop();
    th.join();

    EXPECT_EQ(store->orders, 0);
    EXPECT_EQ(store->fills, 0);
    EXPECT_EQ(store->rejections, 0);
}

TEST(QuestdbWorker, StopFlushesRing)
{
    // After stop(), Worker::run drains remaining events.
    auto store = std::make_shared<MockStore>();
    QuestDbWorker worker(store);
    EventRing ring;

    std::thread th([&] { worker.run(ring); });
    // Push after the worker is already running.
    for (int i = 0; i < 10; ++i)
    {
        auto o = std::make_shared<order_event>(
            std::chrono::system_clock::now(),
            "BTC", order_type::limit, order_side::buy, 1, 1);
        o->set_order_id(static_cast<std::uint64_t>(i + 1));
        ASSERT_TRUE(ring.try_push(o));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    worker.stop();
    th.join();

    EXPECT_EQ(store->orders, 10);
}

#endif // HAS_QUESTDB
