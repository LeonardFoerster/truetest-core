#include <gtest/gtest.h>
#include "core/event_json.h"

#include <string>
#include <chrono>

static auto epoch_ms(int64_t ms)
{
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

// Helper: check that a JSON string contains a key-value substring
static bool json_contains(const std::string& json, const std::string& fragment)
{
    return json.find(fragment) != std::string::npos;
}

TEST(EventJson, MarketEventSerialization)
{
    market_event mkt(epoch_ms(1000), "BTCUSD", 100.5, 105.0, 98.0, 102.0, 5000);
    auto json = event_json::to_json(mkt);

    EXPECT_TRUE(json_contains(json, "\"type\":\"market\""));
    EXPECT_TRUE(json_contains(json, "\"timestamp\":1000"));
    EXPECT_TRUE(json_contains(json, "\"symbol\":\"BTCUSD\""));
    EXPECT_TRUE(json_contains(json, "\"volume\":5000"));
}

TEST(EventJson, OrderEventSerialization)
{
    order_event ord(epoch_ms(2000), "ETHUSD", order_type::limit,
                    order_side::buy, 50, 1500.5);
    ord.set_order_id(42);
    auto json = event_json::to_json(ord);

    EXPECT_TRUE(json_contains(json, "\"type\":\"order\""));
    EXPECT_TRUE(json_contains(json, "\"order_id\":42"));
    EXPECT_TRUE(json_contains(json, "\"symbol\":\"ETHUSD\""));
    EXPECT_TRUE(json_contains(json, "\"side\":\"buy\""));
    EXPECT_TRUE(json_contains(json, "\"order_type\":\"limit\""));
    EXPECT_TRUE(json_contains(json, "\"quantity\":50"));
}

TEST(EventJson, FillEventSerialization)
{
    fill_event fill(epoch_ms(3000), "BTCUSD", 42, order_side::sell, 10, 50000.25, 0.5);
    auto json = event_json::to_json(fill);

    EXPECT_TRUE(json_contains(json, "\"type\":\"fill\""));
    EXPECT_TRUE(json_contains(json, "\"order_id\":42"));
    EXPECT_TRUE(json_contains(json, "\"side\":\"sell\""));
    EXPECT_TRUE(json_contains(json, "\"quantity\":10"));
    EXPECT_TRUE(json_contains(json, "\"commission\":0.5"));
}

TEST(EventJson, TickEventSerialization)
{
    tick_event te(epoch_ms(4000), "SOLUSD", 25.5, 100, tick_side::bid);
    auto json = event_json::to_json(te);

    EXPECT_TRUE(json_contains(json, "\"type\":\"tick\""));
    EXPECT_TRUE(json_contains(json, "\"symbol\":\"SOLUSD\""));
    EXPECT_TRUE(json_contains(json, "\"side\":\"bid\""));
}

TEST(EventJson, PortfolioSerialization)
{
    portfolio p;
    // Simulate a fill to create a position
    fill_event fill(epoch_ms(1000), "TEST", 1, order_side::buy, 10, 100.0, 0.0);
    p.on_fill(fill);

    auto json = event_json::portfolio_to_json(p);

    EXPECT_TRUE(json_contains(json, "\"type\":\"portfolio\""));
    EXPECT_TRUE(json_contains(json, "\"symbol\":\"TEST\""));
    EXPECT_TRUE(json_contains(json, "\"qty\":10"));
}

TEST(EventJson, AnalyticsSerialization)
{
    AnalyticsReport report;
    report.initial_equity = 100000.0;
    report.final_equity = 105000.0;
    report.cumulative_return = 0.05;
    report.sharpe_ratio = 1.5;
    report.max_drawdown = 0.02;
    report.win_rate = 0.6;
    report.total_trades = 42;

    auto json = event_json::analytics_to_json(report);

    EXPECT_TRUE(json_contains(json, "\"type\":\"analytics\""));
    EXPECT_TRUE(json_contains(json, "\"initial_equity\":100000"));
    EXPECT_TRUE(json_contains(json, "\"final_equity\":105000"));
    EXPECT_TRUE(json_contains(json, "\"total_trades\":42"));
}

TEST(EventJson, EventDispatch)
{
    auto mkt = std::make_shared<market_event>(epoch_ms(1000), "TEST", 1, 2, 0.5, 1.5, 100);
    auto json = event_json::event_to_json(mkt);
    EXPECT_TRUE(json_contains(json, "\"type\":\"market\""));

    auto ord = std::make_shared<order_event>(epoch_ms(2000), "TEST",
                                              order_type::market, order_side::buy, 10, 100.0);
    json = event_json::event_to_json(ord);
    EXPECT_TRUE(json_contains(json, "\"type\":\"order\""));

    auto fill = std::make_shared<fill_event>(epoch_ms(3000), "TEST", 1,
                                              order_side::sell, 10, 100.0, 0.0);
    json = event_json::event_to_json(fill);
    EXPECT_TRUE(json_contains(json, "\"type\":\"fill\""));

    auto tick = std::make_shared<tick_event>(epoch_ms(4000), "TEST", 100.0, 50, tick_side::ask);
    json = event_json::event_to_json(tick);
    EXPECT_TRUE(json_contains(json, "\"type\":\"tick\""));
}

TEST(EventJson, UnhandledEventReturnsEmpty)
{
    auto sig = std::make_shared<signal_event>(epoch_ms(1000), "TEST", signal_type::buy);
    auto json = event_json::event_to_json(sig);
    EXPECT_TRUE(json.empty());
}
