#ifdef HAS_QUESTDB

#include "data/questdb/http_client.h"
#include "data/questdb/store.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

namespace tq = truetest::questdb;

namespace {

// Minimal long-extractor for QuestDB's /exec JSON: looks for the first
// integer inside `"dataset":[[<n>` so we can pull COUNT(*) results.
long extract_first_long(const std::string& json)
{
    const std::string marker = "\"dataset\":[[";
    auto pos = json.find(marker);
    if (pos == std::string::npos) return -1;
    pos += marker.size();
    long v = 0;
    bool any = false;
    while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos])))
    {
        v = v * 10 + (json[pos] - '0');
        any = true;
        ++pos;
    }
    return any ? v : -1;
}

long count_rows(const std::string& host, std::uint16_t port,
                const std::string& table)
{
    auto resp = tq::query_exec(host, port, "SELECT COUNT(*) FROM " + table);
    if (!resp || resp->status != 200) return -1;
    return extract_first_long(resp->body);
}

void drop_table(const std::string& host, std::uint16_t port,
                const std::string& table)
{
    (void)tq::query_exec(host, port, "DROP TABLE IF EXISTS " + table);
}

// Build a unique tag per test invocation so re-runs don't collide.
std::string make_tag()
{
    return "itest_" + std::to_string(std::time(nullptr));
}

}

// End-to-end test against a live QuestDB daemon. Skipped (not failed)
// when QUESTDB_TEST_HOST is not set, so CI without the daemon is safe.
TEST(QuestdbIntegration, EndToEndStoreRoundTrip)
{
    const char* host_env = std::getenv("QUESTDB_TEST_HOST");
    if (!host_env || std::strlen(host_env) == 0)
        GTEST_SKIP() << "QUESTDB_TEST_HOST not set - skipping live test.";

    const std::string host = host_env;
    const std::uint16_t http_port = 9000;
    const std::uint16_t ilp_port  = 9009;
    const std::string tag = make_tag();

    // 1. Construct a store directly (skips the engine machinery so the
    //    test is hermetic and fast).
    tq::StoreConfig cfg;
    cfg.host = host;
    cfg.http_port = http_port;
    cfg.ilp_port = ilp_port;
    cfg.run_tag = tag;
    cfg.mode = "backtest";
    cfg.binary = "engine_backtest";
    cfg.strategy = "itest";
    cfg.symbol = "BTCUSDT";
    cfg.initial_equity = 10000.0;
    cfg.notes = "integration test";

    tq::QuestdbStore store(std::move(cfg));
    ASSERT_TRUE(store.begin())
        << "begin() failed - is QuestDB running on " << host
        << ":" << http_port << "?";

    // 2. Emit one of each capture event.
    auto now = std::chrono::system_clock::now();
    order_event order(now, "BTCUSDT", order_type::limit, order_side::buy,
                      0.001, 50000.0);
    order.set_order_id(1);
    order.set_strategy_name("itest");
    store.record_order_submitted(order, "pending");
    store.record_status_transition(1, order_status::pending,
                                   order_status::open);

    fill_event fl(now, "BTCUSDT", 1, order_side::buy, 0.001, 50000.0,
                  /*commission=*/0.05, /*remaining_qty=*/0.0, /*fill_id=*/100);
    store.record_fill(fl, /*opener=*/0, "itest", "simulated");
    store.record_status_transition(1, order_status::open,
                                   order_status::filled);

    order_event rej(now, "BTCUSDT", order_type::market, order_side::sell,
                    0.5, 0.0);
    rej.set_order_id(2);
    rej.set_strategy_name("itest");
    store.record_rejection(rej, "risk_halt", "max_position_value");

    store.record_cancellation(3, "BTCUSDT", "itest", "manual");
    store.record_amendment(4, "BTCUSDT", 50000.0, 50500.0, 0.1, 0.2, now);

    store.end(/*final_equity=*/10100.0,
              /*total_orders=*/2,
              /*total_fills=*/1,
              /*total_rejections=*/1);

    // QuestDB ILP commit is asynchronous - give it a moment to flush to
    // the WAL before we query.
    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    // 3. Verify rows landed.
    const std::string runs_query =
        "SELECT COUNT(*) FROM runs_meta WHERE run_tag='" + tag + "'";
    auto runs = tq::query_exec(host, http_port, runs_query);
    ASSERT_TRUE(runs.has_value());
    EXPECT_GE(extract_first_long(runs->body), 2);

    EXPECT_GE(count_rows(host, http_port, tag + "_orders"), 1);
    EXPECT_GE(count_rows(host, http_port, tag + "_order_status"), 2);
    EXPECT_GE(count_rows(host, http_port, tag + "_fills"), 1);
    EXPECT_GE(count_rows(host, http_port, tag + "_rejections"), 1);
    EXPECT_GE(count_rows(host, http_port, tag + "_cancellations"), 1);
    EXPECT_GE(count_rows(host, http_port, tag + "_amendments"), 1);

    // 4. Cleanup the per-run tables.
    for (const char* suffix : {"_orders", "_order_status", "_fills",
                               "_rejections", "_cancellations",
                               "_amendments"})
    {
        drop_table(host, http_port, tag + suffix);
    }
    (void)tq::query_exec(host, http_port,
        "DELETE FROM runs_meta WHERE run_tag='" + tag + "'");
}

#endif // HAS_QUESTDB
