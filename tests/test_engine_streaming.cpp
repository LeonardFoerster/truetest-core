#include <gtest/gtest.h>
#include "engine/engine.h"
#include "engine/engine_config.h"
#include "data/data_handler.h"
#include "providers/data_bridge.h"
#include "providers/local/csv_parser.h"
#include "test_helpers/mock_transport.h"

#include <sstream>
#include <thread>
#include <chrono>
#include <atomic>

namespace {
// RAII helper to silence cout during noisy engine runs.
// Anonymous namespace: same struct name exists in other test TUs with a
// different layout, and under LTO ODR-merging those definitions crashes at
// teardown. Anonymous-namespace scope gives each TU its own distinct type.
struct SilenceOutput {
    std::ostringstream sink;
    std::streambuf* orig;
    SilenceOutput() : orig(std::cout.rdbuf(sink.rdbuf())) {}
    ~SilenceOutput() { std::cout.rdbuf(orig); }
};
} // namespace

// Test strategy: buys on bar 3, sells on bar 6
class StreamTestStrategy : public IStrategy
{
    bool position_open_ = false;
    int call_count_ = 0;
public:
    int get_call_count() const { return call_count_; }

    std::optional<order_event> on_market(const market_event& mkt) override
    {
        call_count_++;
        if (call_count_ == 3 && !position_open_)
        {
            return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                               order_type::market, order_side::buy, 10, mkt.get_close());
        }
        if (call_count_ == 6 && position_open_)
        {
            return order_event(mkt.get_timestamp(), mkt.get_symbol(),
                               order_type::market, order_side::sell, 10, mkt.get_close());
        }
        return std::nullopt;
    }
    void set_position_open(const std::string&, bool open) override { position_open_ = open; }
};

// Tick-counting strategy: just counts ticks, never trades
class TickCountStrategy : public IStrategy
{
    std::atomic<int> tick_count_{0};
public:
    int get_tick_count() const { return tick_count_.load(); }

    std::optional<order_event> on_market(const market_event&) override
    {
        return std::nullopt;
    }

    std::optional<order_event> on_tick(const tick_event&) override
    {
        tick_count_++;
        return std::nullopt;
    }

    void set_position_open(const std::string&, bool) override {}
};

static std::vector<std::string> make_bar_lines(int n)
{
    // Header + n data lines
    std::vector<std::string> lines;
    lines.push_back("date,symbol,open,high,low,close,volume");
    for (int i = 0; i < n; ++i)
    {
        lines.push_back("2024-01-01,TEST," +
            std::to_string(100.0 + i) + "," +
            std::to_string(105.0 + i) + "," +
            std::to_string(95.0 + i) + "," +
            std::to_string(102.0 + i) + ",1000");
    }
    return lines;
}

static std::vector<std::string> make_tick_lines(int n)
{
    std::vector<std::string> lines;
    lines.push_back("timestamp_ms,symbol,price,quantity,side");
    for (int i = 0; i < n; ++i)
    {
        lines.push_back(std::to_string(1000 + i * 100) + ",TEST," +
            std::to_string(100.0 + i * 0.1) + "," +
            std::to_string(10 + i) + ",B");
    }
    return lines;
}

// --- Bar streaming tests ---

TEST(EngineStreaming, BarStreamProcessesAllRecords)
{
    SilenceOutput silence;

    auto transport = std::make_shared<MockStreamingTransport>();
    auto parser = std::make_shared<CsvBarParser>();
    auto bridge = std::make_shared<DataBridge<bar_record>>(transport, parser, bar_record_sink);

    auto dh = std::make_shared<data_handler>();
    auto strategy = std::make_shared<StreamTestStrategy>();

    engine_config cfg;
    cfg.seed = 42;
    engine eng(dh, nullptr, strategy, std::move(cfg));

    auto lines = make_bar_lines(10);

    // Feed data from a background thread
    std::thread feeder([&]() {
        // Give the engine a moment to start
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        for (const auto& line : lines)
        {
            transport->enqueue(line);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        // Signal end of data
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        transport->request_stop();
    });

    eng.run_streaming(bridge);
    feeder.join();

    // All 10 bar records should have been fed to data_handler
    EXPECT_EQ(dh->db_data_symbol.size(), 10u);
    // Strategy should have been called 10 times
    EXPECT_EQ(strategy->get_call_count(), 10);
}

TEST(EngineStreaming, BarStreamStrategyGeneratesSignals)
{
    SilenceOutput silence;

    auto transport = std::make_shared<MockStreamingTransport>();
    auto parser = std::make_shared<CsvBarParser>();
    auto bridge = std::make_shared<DataBridge<bar_record>>(transport, parser, bar_record_sink);

    auto dh = std::make_shared<data_handler>();
    auto strategy = std::make_shared<StreamTestStrategy>();

    engine_config cfg;
    cfg.seed = 42;
    engine eng(dh, nullptr, strategy, std::move(cfg));

    auto lines = make_bar_lines(10);

    std::thread feeder([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        for (const auto& line : lines)
        {
            transport->enqueue(line);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        transport->request_stop();
    });

    eng.run_streaming(bridge);
    feeder.join();

    // Strategy buys at bar 3 and sells at bar 6 — should see trades
    EXPECT_GT(strategy->get_call_count(), 5);
}

TEST(EngineStreaming, BarStreamStopCausesReturn)
{
    SilenceOutput silence;

    auto transport = std::make_shared<MockStreamingTransport>();
    auto parser = std::make_shared<CsvBarParser>();
    auto bridge = std::make_shared<DataBridge<bar_record>>(transport, parser, bar_record_sink);

    auto dh = std::make_shared<data_handler>();
    auto strategy = std::make_shared<StreamTestStrategy>();

    engine_config cfg;
    cfg.seed = 42;
    engine eng(dh, nullptr, strategy, std::move(cfg));

    // Enqueue header only, then stop after a short delay
    transport->enqueue("date,symbol,open,high,low,close,volume");

    std::thread stopper([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        bridge->stop();
    });

    auto start = std::chrono::steady_clock::now();
    eng.run_streaming(bridge);
    auto elapsed = std::chrono::steady_clock::now() - start;
    stopper.join();

    // Should return promptly (< 500ms)
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 500);
    // No data should have been processed
    EXPECT_EQ(dh->db_data_symbol.size(), 0u);
}

TEST(EngineStreaming, BarStreamBatchTransportFallback)
{
    SilenceOutput silence;

    // Use MockBatchTransport (non-streaming) through run_streaming —
    // it should still work since DataBridge::run_streaming falls back to read_line()
    auto lines = make_bar_lines(5);
    auto transport = std::make_shared<MockBatchTransport>(lines);
    auto parser = std::make_shared<CsvBarParser>();
    auto bridge = std::make_shared<DataBridge<bar_record>>(transport, parser, bar_record_sink);

    auto dh = std::make_shared<data_handler>();
    auto strategy = std::make_shared<StreamTestStrategy>();

    engine_config cfg;
    cfg.seed = 42;
    engine eng(dh, nullptr, strategy, std::move(cfg));

    eng.run_streaming(bridge);

    EXPECT_EQ(dh->db_data_symbol.size(), 5u);
    EXPECT_EQ(strategy->get_call_count(), 5);
}

// --- Tick streaming tests ---

TEST(EngineStreaming, TickStreamProcessesAllRecords)
{
    SilenceOutput silence;

    auto transport = std::make_shared<MockStreamingTransport>();
    auto parser = std::make_shared<CsvTickParser>();
    auto bridge = std::make_shared<DataBridge<tick_record>>(transport, parser, tick_record_sink);

    auto dh = std::make_shared<data_handler>();
    auto strategy = std::make_shared<TickCountStrategy>();

    engine_config cfg;
    cfg.seed = 42;
    engine eng(dh, nullptr, strategy, std::move(cfg));

    auto lines = make_tick_lines(8);

    // Pre-enqueue header so run_streaming's read_line() finds it immediately
    transport->enqueue(lines[0]);

    std::thread feeder([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        for (size_t i = 1; i < lines.size(); ++i)
        {
            transport->enqueue(lines[i]);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        transport->request_stop();
    });

    eng.run_streaming(bridge);
    feeder.join();

    // All 8 tick records should have been fed to data_handler
    EXPECT_EQ(dh->tick_data.size(), 8u);
    EXPECT_EQ(strategy->get_tick_count(), 8);
}

TEST(EngineStreaming, TickStreamStopCausesReturn)
{
    SilenceOutput silence;

    auto transport = std::make_shared<MockStreamingTransport>();
    auto parser = std::make_shared<CsvTickParser>();
    auto bridge = std::make_shared<DataBridge<tick_record>>(transport, parser, tick_record_sink);

    auto dh = std::make_shared<data_handler>();
    auto strategy = std::make_shared<TickCountStrategy>();

    engine_config cfg;
    cfg.seed = 42;
    engine eng(dh, nullptr, strategy, std::move(cfg));

    // Enqueue header only
    transport->enqueue("timestamp_ms,symbol,price,quantity,side");

    std::thread stopper([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        bridge->stop();
    });

    auto start = std::chrono::steady_clock::now();
    eng.run_streaming(bridge);
    auto elapsed = std::chrono::steady_clock::now() - start;
    stopper.join();

    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 500);
    EXPECT_EQ(dh->tick_data.size(), 0u);
}
