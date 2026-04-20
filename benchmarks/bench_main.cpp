// O3: Performance benchmarks for the core hot-path components.
//
// Build: cmake -B build -DENABLE_BENCHMARKS=ON && cmake --build build --target truetest_benchmarks
// Run:   ./build/truetest_benchmarks
//        ./build/truetest_benchmarks --benchmark_filter=Orderbook
//        ./build/truetest_benchmarks --benchmark_format=json > results.json
//
// Covers: orderbook insert+match, SPSC ring buffer push/pop, event JSON
// serialization, SMA indicator update, and full engine throughput on a
// synthetic 100K-bar dataset.

#include <benchmark/benchmark.h>

#include "engine/engine.h"
#include "engine/engine_config.h"
#include "core/event.h"
#include "engine/event_json.h"
#include "data/data_handler.h"
#include "indicator/sma.h"
#include "market_maker/market_maker.h"
#include "orderbook/orderbook.h"
#include "strategy/sma_strategy.h"
#include "threading/ring_buffer.h"
#include "types/price.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

// Silence engine stdout during benchmark runs.
struct silence_cout
{
    std::ostringstream sink;
    std::streambuf* orig;
    silence_cout() : sink(), orig(std::cout.rdbuf(sink.rdbuf())) {}
    ~silence_cout() { std::cout.rdbuf(orig); }
};

std::shared_ptr<order> make_order(order_id id, side s, double price, quantity qty)
{
    return std::make_shared<order>(
        ob_order_type::good_till_cancel, id, s, Price::from_double(price), qty);
}

} // namespace

// ─── Orderbook: insert non-crossing orders, then cancel ─────────────────────
// Measures the cost of adding a resting order and immediately cancelling it,
// which exercises the flat-array price-level path plus the free-list node
// pool. Non-crossing so no matching occurs.
static void BM_Orderbook_InsertCancel(benchmark::State& state)
{
    orderbook ob;
    uint64_t next_id = 1;
    for (auto _ : state)
    {
        // Alternate prices across 100 levels around 100.0 to touch the
        // sorted-array insertion path without triggering a match.
        double p = 99.0 - (next_id % 100) * 0.01;
        auto o = make_order(next_id, side::buy, p, 10);
        auto trades = ob.add_order(o);
        benchmark::DoNotOptimize(trades);
        ob.cancel_order(next_id);
        ++next_id;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Orderbook_InsertCancel);

// ─── Orderbook: crossing order triggers a match ─────────────────────────────
// Pre-seeds a book with 1000 resting asks, then benchmarks inserting an
// aggressive bid that crosses and matches one level.
static void BM_Orderbook_Match(benchmark::State& state)
{
    orderbook ob;
    uint64_t next_id = 1;

    // Seed 1000 ask levels at 100.01 … 110.00 (step 0.01), each with 10 qty.
    for (int i = 0; i < 1000; ++i)
    {
        double px = 100.01 + i * 0.01;
        ob.add_order(make_order(next_id++, side::sell, px, 10));
    }

    uint64_t aggressor_id = 1'000'000;
    for (auto _ : state)
    {
        state.PauseTiming();
        // Reset the single level we just drained
        ob.add_order(make_order(next_id++, side::sell, 100.01, 10));
        state.ResumeTiming();

        auto trades = ob.add_order(
            make_order(aggressor_id++, side::buy, 100.01, 10));
        benchmark::DoNotOptimize(trades);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Orderbook_Match);

// ─── Ring buffer: single-threaded push + pop round trip ─────────────────────
static void BM_RingBuffer_PushPop(benchmark::State& state)
{
    RingBuffer<int, 1024> ring;
    int v = 0;
    for (auto _ : state)
    {
        ring.try_push(v);
        int out = 0;
        ring.try_pop(out);
        benchmark::DoNotOptimize(out);
        ++v;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RingBuffer_PushPop);

// ─── Event JSON serialization: market event ─────────────────────────────────
static void BM_EventJson_Market(benchmark::State& state)
{
    market_event mkt(
        std::chrono::system_clock::now(),
        "BTCUSDT", 100.0, 101.0, 99.5, 100.5, 1234);
    for (auto _ : state)
    {
        auto s = event_json::to_json(mkt);
        benchmark::DoNotOptimize(s);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_EventJson_Market);

// ─── Event JSON serialization: fill event ───────────────────────────────────
static void BM_EventJson_Fill(benchmark::State& state)
{
    fill_event f(
        std::chrono::system_clock::now(),
        "BTCUSDT",
        /*order_id=*/42, order_side::buy,
        /*filled_qty=*/1.5, /*fill_price=*/100.25,
        /*commission=*/0.1, /*remaining=*/0.0,
        /*fill_id=*/1);
    for (auto _ : state)
    {
        auto s = event_json::to_json(f);
        benchmark::DoNotOptimize(s);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_EventJson_Fill);

// ─── SMA indicator update throughput ────────────────────────────────────────
static void BM_SMA_Update(benchmark::State& state)
{
    simple_moving_average sma(20);
    double x = 100.0;
    for (auto _ : state)
    {
        // Oscillate around 100 so the SMA sees realistic variance.
        x = 100.0 + std::sin(x) * 0.5;
        auto v = sma.update(x);
        benchmark::DoNotOptimize(v);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SMA_Update);

// ─── Full engine throughput: 100K synthetic bars ────────────────────────────
// Each iteration runs one engine over a pre-built 100K bar dataset. Reports
// bars/sec via SetItemsProcessed (iterations × 100K).
static void BM_Engine_Throughput_100k(benchmark::State& state)
{
    constexpr std::size_t N_BARS = 100'000;

    // Build the dataset once, reuse across iterations.
    auto build_handler = [] {
        auto dh = std::make_shared<data_handler>();
        dh->db_data_date.reserve(N_BARS);
        dh->db_data_symbol.reserve(N_BARS);
        dh->db_data_open_value.reserve(N_BARS);
        dh->db_data_high_value.reserve(N_BARS);
        dh->db_data_low_value.reserve(N_BARS);
        dh->db_data_close_value.reserve(N_BARS);
        dh->db_data_volume_value.reserve(N_BARS);

        double p = 100.0;
        for (std::size_t i = 0; i < N_BARS; ++i)
        {
            const double drift = std::sin(i * 0.01) * 0.5;
            const double c = p + drift;
            const double o = p;
            const double h = std::max(o, c) + 0.1;
            const double l = std::min(o, c) - 0.1;

            char date_buf[32];
            std::snprintf(date_buf, sizeof(date_buf),
                          "2024-01-01T%02zu:%02zu:%02zu",
                          (i / 3600) % 24, (i / 60) % 60, i % 60);

            dh->db_data_date.emplace_back(date_buf);
            dh->db_data_symbol.emplace_back("BENCH");
            dh->db_data_open_value.push_back(o);
            dh->db_data_high_value.push_back(h);
            dh->db_data_low_value.push_back(l);
            dh->db_data_close_value.push_back(c);
            dh->db_data_volume_value.push_back(1000);

            p = c;
        }
        return dh;
    };

    auto dh_template = build_handler();

    silence_cout quiet;
    for (auto _ : state)
    {
        // Fresh orderbook + strategy per iteration for clean state. The
        // data_handler is copied cheaply (shared_ptr to pre-built vectors).
        auto dh = std::make_shared<data_handler>(*dh_template);
        auto ob = std::make_shared<orderbook>();
        MarketMaker mm(424242u);
        mm.add_orders(ob, 100.0, 40);

        auto strat = std::make_shared<sma_strategy>(20);

        engine_config cfg;
        cfg.initial_balance = 100000.0;
        cfg.seed            = 424242;
        cfg.threading       = thread_preset::inline_mode;
        cfg.disable_pinning = true;

        engine eng(dh, ob, strat, cfg);
        eng.run();
    }
    state.SetItemsProcessed(state.iterations() *
                            static_cast<int64_t>(N_BARS));
}
// Run with fewer iterations — one full run is already 100K bars.
BENCHMARK(BM_Engine_Throughput_100k)->Unit(benchmark::kMillisecond)->Iterations(3);

BENCHMARK_MAIN();
