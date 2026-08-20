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
#include "data/data_handler.h"
#include "execution/execution_adapter.h"
#include "indicator/sma.h"
#include "market_maker/market_maker.h"
#include "orderbook/orderbook.h"
#include "risk/risk_accounting.h"
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
#include <unordered_map>
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
        const double p =
            99.0 - static_cast<double>(next_id % 100U) * 0.01;
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

// LocalBookAdapter bar-range scan. No order is traversed, so this isolates
// the per-resting-order scan cost.
static void BM_LocalBookAdapter_RangeMiss(benchmark::State& state)
{
    auto ob = std::make_shared<orderbook>();
    LocalBookAdapter adapter(ob, nullptr, nullptr, 42, 1.1, 1.0);
    const auto ts = std::chrono::system_clock::time_point{};
    const auto order_count = static_cast<std::size_t>(state.range(0));

    for (std::size_t i = 0; i < order_count; ++i)
    {
        order_event o(ts, "X", order_type::limit, order_side::buy, 1.0, 90.0);
        o.set_order_id(static_cast<std::uint64_t>(i + 1));
        o.set_earliest_eligible_ts(ts);
        adapter.submit_order(o);
    }

    for (auto _ : state)
    {
        benchmark::DoNotOptimize(
            adapter.sweep_resting_range("X", 99.0, 101.0, ts));
    }
    state.SetItemsProcessed(
        state.iterations() * static_cast<std::int64_t>(order_count));
}
BENCHMARK(BM_LocalBookAdapter_RangeMiss)->Arg(1)->Arg(16)->Arg(128)->Arg(1000);

// Volume consumes exactly one resting order; setup replenishes it outside
// timing so every iteration observes the same live-order cardinality.
static void BM_LocalBookAdapter_VolumeCappedHead(benchmark::State& state)
{
    auto ob = std::make_shared<orderbook>();
    LocalBookAdapter adapter(ob, nullptr, nullptr, 42, 1.1, 1.0);
    const auto ts = std::chrono::system_clock::time_point{};
    const auto order_count = static_cast<std::size_t>(state.range(0));
    std::uint64_t next_id = 1;

    const auto submit_one = [&] {
        order_event o(ts, "X", order_type::limit,
                      order_side::buy, 1.0, 99.5);
        o.set_order_id(next_id++);
        o.set_earliest_eligible_ts(ts);
        adapter.submit_order(o);
    };
    for (std::size_t i = 0; i < order_count; ++i) submit_one();

    std::vector<fill_event> fills;
    fills.reserve(1);
    for (auto _ : state)
    {
        benchmark::DoNotOptimize(
            adapter.sweep_resting_range("X", 99.0, 101.0, ts, 1.0));

        state.PauseTiming();
        fills.clear();
        adapter.poll_fills(fills);
        submit_one();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_LocalBookAdapter_VolumeCappedHead)
    ->Arg(1)->Arg(16)->Arg(128)->Arg(1000);

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

// ─── (Event JSON benchmarks removed — event_json.h no longer exists)
//     If you need JSON microbenchmarks, implement using current json_emit or nlohmann.

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
        dh->reserve_bars(N_BARS);

        double p = 100.0;
        for (std::size_t i = 0; i < N_BARS; ++i)
        {
            const double drift =
                std::sin(static_cast<double>(i) * 0.01) * 0.5;
            const double c = p + drift;
            const double o = p;
            const double h = std::max(o, c) + 0.1;
            const double l = std::min(o, c) - 0.1;

            char date_buf[32];
            std::snprintf(date_buf, sizeof(date_buf),
                          "2024-01-01T%02zu:%02zu:%02zu",
                          (i / 3600) % 24, (i / 60) % 60, i % 60);

            dh->load_into_queue(date_buf, "BENCH", o, h, l, c, 1000);

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
        cfg.pool_prewarm.orderbook_order_blocks = 50;  // extra safety for benchmark
        cfg.pool_prewarm.forbid_runtime_grow = false;  // allow grow for benchmark stability

        engine eng(dh, ob, strat, cfg);
        eng.run();
    }
    state.SetItemsProcessed(state.iterations() *
                            static_cast<int64_t>(N_BARS));
}
// Run with fewer iterations — one full run is already 100K bars.
BENCHMARK(BM_Engine_Throughput_100k)->Unit(benchmark::kMillisecond)->Iterations(3);

// ─── R3: authoritative order ledger + risk view (order hot path) ────────────
// Both run once per candidate order inside OrderIntentProcessor::process, so
// they sit directly on the order hot path. Budget: allocation-free steady
// state and O(#symbols) — never O(#orders ever seen). See
// docs/internal/r3-authoritative-risk-accounting.md.

// One full lifecycle (register -> pending -> open -> partial fill -> fill)
// through the ledger, including the incremental per-symbol aggregates.
static void BM_RiskLedger_Lifecycle(benchmark::State& state)
{
    const auto ts = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(1'000));
    OrderTracker ledger;
    ledger.reserve(4096);

    std::uint64_t id = 0;
    for (auto _ : state)
    {
        ++id;
        order_event o(ts, "BENCHUSDT", order_type::limit, order_side::buy,
                      10.0, 100.0);
        o.set_order_id(id);
        ledger.register_order(o);
        ledger.set_status(id, order_status::pending);
        ledger.set_status(id, order_status::open);
        ledger.on_fill(fill_event(ts, "BENCHUSDT", id, order_side::buy,
                                  4.0, 100.0, 0.0, 6.0));
        ledger.on_fill(fill_event(ts, "BENCHUSDT", id, order_side::buy,
                                  6.0, 100.0, 0.0, 0.0));
        benchmark::DoNotOptimize(ledger.active_count());
        // The ledger keeps one record per order id ever seen; reset before it
        // dominates the measurement with pure map growth.
        if (ledger.orders_seen() > 100'000)
        {
            state.PauseTiming();
            ledger.reset();
            ledger.reserve(4096);
            state.ResumeTiming();
        }
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RiskLedger_Lifecycle)->Unit(benchmark::kNanosecond);

// Building the authoritative risk snapshot for one candidate order, over a
// portfolio of `state.range(0)` instruments each carrying resting orders.
static void BM_RiskView_Build(benchmark::State& state)
{
    const auto ts = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(1'000));
    const int symbols = static_cast<int>(state.range(0));

    OrderTracker ledger;
    ledger.reserve(1024);
    portfolio port(1'000'000.0);
    std::unordered_map<std::string, mark_point> marks;
    std::vector<std::string> names;
    names.reserve(static_cast<std::size_t>(symbols));

    std::uint64_t id = 0;
    for (int i = 0; i < symbols; ++i)
    {
        names.push_back("SYM" + std::to_string(i));
        const auto& sym = names.back();
        marks[sym] = mark_point{100.0 + i, ts};
        port.on_fill(fill_event(ts, sym, ++id, order_side::buy, 3.0, 100.0, 0.0));
        for (int k = 0; k < 4; ++k)
        {
            order_event o(ts, sym, order_type::limit,
                          (k % 2 == 0) ? order_side::buy : order_side::sell,
                          2.0, 100.0);
            o.set_order_id(++id);
            ledger.register_order(o);
            ledger.set_status(o.get_order_id(), order_status::open);
        }
    }

    const auto mark_for = [&](const std::string& sym) -> mark_point {
        auto it = marks.find(sym);
        return (it != marks.end()) ? it->second : mark_point{};
    };

    risk_snapshot snap;
    for (auto _ : state)
    {
        truetest::risk::build_risk_view(snap, names.front(), port, ledger,
                                        ts, /*max_mark_age_ms=*/1'000, mark_for);
        benchmark::DoNotOptimize(snap.portfolio.gross_exposure);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RiskView_Build)->Arg(1)->Arg(4)->Arg(16)->Unit(benchmark::kNanosecond);

BENCHMARK_MAIN();
