# Multithreading Implementation Guide

Instructions for Claude Code: execute each step in order. After each step,
build with `cmake -B build -DBUILD_TESTS=ON && cmake --build build` and run
`cd build && ctest --output-on-failure` to verify nothing is broken. Do not
skip steps — later steps depend on earlier ones.

Reference: `BacktestEngine/docs/multithreading.md` for the high-level
architecture diagram and design rationale.

---

## Current State (what already exists)

Before you start, understand what's already built:

### Infrastructure (fully implemented, tested)
- **`threading/ring_buffer.h`** — lock-free SPSC ring buffer with 3 backpressure
  policies (`SpinWait`, `DropOldest`, `AssertFull`). Cache-line-padded atomics.
  Power-of-2 capacity. Tested in `tests/test_ring_buffer.cpp`.
- **`threading/worker.h`** — `Worker` base class with `on_event()` pure virtual,
  templated `run()` that spins on a ring buffer, `stop()` + drain semantics.
- **`threading/thread_config.h`** — hardware topology detection via sysfs,
  `detect_physical_cores()`, `get_physical_core_ids()` (avoids HT siblings),
  `build_core_map()` (assigns roles to physical cores using scaling table),
  `pin_to_core()` (pthread affinity, no-op on non-Linux).
- **`types/object_pool.h`** — mutex-protected pool with custom `shared_ptr`
  deleter. Pre-allocates 4096-slot blocks. Used in engine for market/order/
  fill/tick event allocation.

### Workers (stub implementations, wired to engine)
- **`threading/logging_worker.h`** — `LoggingWorker`: counts events, does NOT
  actually write to any log sink. The `on_event()` is a no-op stub.
- **`threading/risk_worker.h`** — `RiskWorker`: receives events, runs
  `risk_manager_.check_order()` / `check_post_fill()` on them, sets
  `halt_flag_` if halt is needed. **DATA RACE**: reads `portfolio_` and
  `analytics_` (const refs) that Core 0 mutates concurrently.
- **`threading/stats_worker.h`** — `StatsWorker`: feeds events to a
  `Analytics` instance, takes periodic snapshots. **DATA RACE**: calls
  `analytics_.on_event()` on the SAME `Analytics` object that Core 0 also
  calls `on_event()` on.

### Engine integration (partially wired)
- `engine_config::enable_threading` flag (default false).
- Engine creates 3 ring buffers (`logging_ring_`, `risk_ring_`, `stats_ring_`)
  when threading is enabled.
- `engine::publish_event()` does `try_push()` into all 3 rings.
- `engine::start_workers()` spawns 3 threads, pins them via `build_core_map()`.
- `engine::stop_workers()` signals stop, joins threads.
- Tests exist in `tests/test_engine.cpp`: `ThreadingEnabled_WorkersRun`,
  `HaltChannel_StopsEngine`, `GracefulShutdown_RingsDrained`.

### Critical problems to fix
1. **Shared mutable state**: `Analytics`, `portfolio`, and `RiskManager` are
   shared between Core 0 and worker threads without synchronisation. This is
   undefined behaviour (data races).
2. **All-or-nothing threading**: `enable_threading = true` spawns all workers
   regardless of available cores. On a 2-core machine this causes 5 threads
   fighting for 2 cores — worse than single-threaded.
3. **Missing market maker worker**: `core_role::market_maker` exists in the
   enum and core map but no `MarketMakerWorker` class exists. The market maker
   runs inline on Core 0.
4. **Logging worker is a stub**: does nothing useful.
5. **No inbound ring for Core 0**: the design doc specifies that the market
   maker pushes orders BACK to Core 0, but no inbound ring exists.
6. **No per-worker Analytics**: `StatsWorker` shares the engine's `Analytics`
   object. It needs its own copy that receives events independently.

---

## Design: Preset-Based Thread Scaling

The central idea: instead of a single `enable_threading` boolean, define
**thread presets** that match the workload to available hardware. The engine
detects physical core count at startup via `detect_physical_cores()` (already
implemented in `thread_config.h`) and selects the appropriate preset
automatically. Users can override with `--thread-preset <name>` or
`--threads <N>`.

### Preset Definitions

```
┌────────────────────────────────────────────────────────────────────────────┐
│  INLINE (1-2 cores)                                                      │
│                                                                          │
│  ┌──────────────────────────────────────────────────┐                    │
│  │  Thread 0 (only thread)                          │                    │
│  │  event loop + strategy + orderbook + portfolio   │                    │
│  │  + analytics + risk + market maker + logging     │                    │
│  └──────────────────────────────────────────────────┘                    │
│                                                                          │
│  Rings: 0           Workers: 0          Threads: 1                       │
│  Behaviour: identical to current single-threaded mode                    │
└────────────────────────────────────────────────────────────────────────────┘

┌────────────────────────────────────────────────────────────────────────────┐
│  LIGHT (3 cores)                                                         │
│                                                                          │
│  ┌──────────────────────────┐     SPSC     ┌─────────────────────────┐   │
│  │  Thread 0 — HOT PATH    │ ──────────> │  Thread 1 — OBSERVER    │   │
│  │  event loop + strategy   │              │  risk + stats + logging │   │
│  │  orderbook + portfolio   │ <────────── │  (halt channel)         │   │
│  │  market maker (inline)   │   atomic     └─────────────────────────┘   │
│  └──────────────────────────┘                                            │
│                                                                          │
│  Rings: 1 outbound          Workers: 1 (ObserverWorker)  Threads: 2     │
│  Key: all non-critical work on a single observer thread. Market maker    │
│  stays inline because we can't spare a thread for it.                    │
└────────────────────────────────────────────────────────────────────────────┘

┌────────────────────────────────────────────────────────────────────────────┐
│  STANDARD (4-5 cores)                                                    │
│                                                                          │
│  ┌──────────────────────────┐                                            │
│  │  Thread 0 — HOT PATH    │──── SPSC ──> [Thread 1: LOGGING]           │
│  │  event loop + strategy   │──── SPSC ──> [Thread 2: RISK + STATS]     │
│  │  orderbook + portfolio   │                                            │
│  │  market maker (inline)   │<── atomic ── [Thread 2: halt channel]     │
│  └──────────────────────────┘                                            │
│                                                                          │
│  Rings: 2 outbound          Workers: 2                   Threads: 3     │
│  Key: logging gets its own thread (I/O isolation). Risk and stats share  │
│  a thread via a combined RiskStatsWorker. Market maker stays inline.     │
└────────────────────────────────────────────────────────────────────────────┘

┌────────────────────────────────────────────────────────────────────────────┐
│  FULL (6-7 cores)                                                        │
│                                                                          │
│  ┌──────────────────────────┐                                            │
│  │  Thread 0 — HOT PATH    │──── SPSC ──> [Thread 1: LOGGING]           │
│  │  event loop + strategy   │──── SPSC ──> [Thread 2: RISK]             │
│  │  orderbook + portfolio   │──── SPSC ──> [Thread 3: STATS]            │
│  │  market maker (inline)   │                                            │
│  └──────────────────────────┘<── atomic ── [Thread 2: halt channel]     │
│                                                                          │
│  Rings: 3 outbound          Workers: 3                   Threads: 4     │
│  Key: risk and stats each get their own thread. Risk checking runs at    │
│  full speed without stats computation competing for cycles.              │
└────────────────────────────────────────────────────────────────────────────┘

┌────────────────────────────────────────────────────────────────────────────┐
│  EXTENDED (8+ cores)                                                     │
│                                                                          │
│  ┌──────────────────────────┐                                            │
│  │  Thread 0 — HOT PATH    │──── SPSC ──> [Thread 1: LOGGING]           │
│  │  event loop + strategy   │──── SPSC ──> [Thread 2: RISK]             │
│  │  orderbook + portfolio   │──── SPSC ──> [Thread 3: STATS]            │
│  │                          │──── SPSC ──> [Thread 4: MARKET MAKER]     │
│  └──────────────────────────┘<── atomic ── [Thread 2: halt channel]     │
│                              <── SPSC ─── [Thread 4: MM order ring]     │
│                                                                          │
│  Rings: 4 outbound + 1 inbound   Workers: 4             Threads: 5     │
│  Key: market maker moves off the hot path. It produces liquidity orders  │
│  asynchronously via an inbound ring that Core 0 drains each iteration.   │
└────────────────────────────────────────────────────────────────────────────┘
```

### Why these tiers

- **Inline → Light** (2 → 3 cores): the single biggest win. Risk checking
  and analytics move off the hot path. One ring, one thread, measurable
  latency improvement.
- **Light → Standard** (3 → 4-5): logging I/O isolation. Disk writes (event
  log, structured logs) can block for milliseconds. Separating logging from
  risk/stats prevents I/O stalls from delaying risk checks.
- **Standard → Full** (5 → 6-7): risk gets its own thread. Under heavy load
  (many symbols, complex risk limits), risk checking can take microseconds per
  event. Sharing a thread with stats means risk checks queue behind stats
  computation.
- **Full → Extended** (7 → 8+): market maker moves off Core 0. This is the
  final piece from the design doc. The market maker's volatility calculations
  and order generation no longer add latency to the strategy → fill path.

### Selection logic

```cpp
thread_preset select_preset(std::size_t physical_cores)
{
    if (physical_cores <= 2) return thread_preset::inline_mode;
    if (physical_cores <= 3) return thread_preset::light;
    if (physical_cores <= 5) return thread_preset::standard;
    if (physical_cores <= 7) return thread_preset::full;
    return thread_preset::extended;
}
```

The user can force any preset regardless of detected cores. This is important
for testing (run Extended on a 4-core machine to verify correctness) and for
deployments where `hardware_concurrency()` reports wrong values (containers,
VMs, cgroups).

---

## Step MT-0 — Preset Enum, Config, and Selection

**Goal:** Replace `bool enable_threading` with preset-based configuration.
No threading behaviour changes yet — this step is pure plumbing.

### MT-0a — Define the preset enum and selection function

**Create `threading/thread_preset.h`:**

```cpp
#pragma once
#include <cstddef>
#include <string>

enum class thread_preset
{
    inline_mode,  // 1-2 cores: no worker threads
    light,        // 3 cores:   1 combined observer worker
    standard,     // 4-5 cores: logging + combined risk/stats worker
    full,         // 6-7 cores: logging + risk + stats (separate)
    extended      // 8+ cores:  logging + risk + stats + market maker
};

// Select preset based on physical core count.
// Returns inline_mode if physical_cores <= 2, etc.
thread_preset select_preset(std::size_t physical_cores);

// Convert preset to/from string for CLI/logging.
std::string preset_to_string(thread_preset p);
thread_preset string_to_preset(const std::string& s);  // throws on bad input

// How many worker threads does this preset spawn?
int preset_worker_count(thread_preset p);

// Does this preset offload the market maker to its own thread?
bool preset_has_mm_worker(thread_preset p);

// Does this preset have a separate risk thread (vs. combined observer)?
bool preset_has_separate_risk(thread_preset p);

// Does this preset have a separate logging thread?
bool preset_has_separate_logging(thread_preset p);
```

Implement in `threading/thread_preset.cpp`. Add to CMakeLists.txt SOURCES.

### MT-0b — Update engine_config

Replace the boolean with the preset and add override options:

```cpp
struct engine_config
{
    // ... existing fields ...

    // Threading preset: auto-detected from hardware_concurrency() by default.
    // Set to inline_mode to disable all threading.
    thread_preset threading = thread_preset::inline_mode;

    // Override: if true, skip all CPU affinity/pinning calls.
    // Useful in containers or VMs where reported topology is wrong.
    bool disable_pinning = false;

    // Override: explicit core IDs per role. -1 = auto from build_core_map().
    int pin_event_loop = -1;
    int pin_logging    = -1;
    int pin_risk       = -1;
    int pin_stats      = -1;
    int pin_mm         = -1;
};
```

Remove the old `bool enable_threading` field. Add a helper:

```cpp
bool is_threaded() const { return threading != thread_preset::inline_mode; }
```

### MT-0c — Update all call sites

Search for every reference to `config_.enable_threading` in the codebase
and replace with `config_.is_threaded()`. This includes:
- `engine.cpp`: constructor (ring creation), `publish_event()`,
  `start_workers()`, `stop_workers()`, inline analytics guards, inline risk
  guards.
- `engine.h`: ring creation in constructor.

The existing `start_workers()` / `stop_workers()` still spawn all 3 workers
for now — that changes in MT-2. The purpose of this step is to get the config
plumbing right without changing behaviour.

### MT-0d — Auto-detect preset in main.cpp

In `main()`, after parsing CLI args, auto-detect the preset:

```cpp
auto cores = detect_physical_cores();
config.threading = select_preset(cores);
std::cout << "  Detected " << cores << " physical cores → preset: "
          << preset_to_string(config.threading) << "\n";
```

Add CLI override: `--thread-preset <inline|light|standard|full|extended>`
and `--no-pin`.

### Tests
- Unit test `select_preset()`: verify each core count maps to the correct
  preset. Test edge cases (0, 1, 2, 3, 5, 7, 8, 64).
- Unit test `preset_to_string()` / `string_to_preset()` round-trip.
- Update existing threading tests to use `config.threading = thread_preset::full`
  instead of `config.enable_threading = true`.
- Verify all 202+ existing tests still pass.

---

## Step MT-1 — Fix Data Races (shared mutable state)

**Problem:** This is the most critical issue. `RiskWorker` reads `portfolio_`
and `analytics_` via const refs while Core 0 mutates them concurrently.
`StatsWorker` calls `analytics_.on_event()` on the same `Analytics` object
Core 0 uses. These are data races = undefined behaviour. Must be fixed
before any preset beyond `inline_mode` is safe to use.

### MT-1a — Give StatsWorker its own Analytics instance

The stats worker must own a **separate** `Analytics` object that receives
events exclusively from its ring. Core 0 keeps its own inline `Analytics`
(used in inline_mode, and as the engine's `print_summary()` source).

**Changes to `threading/stats_worker.h`:**
- Change `Analytics& analytics_` to `Analytics analytics_` (owned, not a
  reference). Construct internally with the same `initial_cash` (default
  100000.0).
- Remove the `Analytics&` constructor parameter. Constructor becomes:
  `explicit StatsWorker(std::size_t snapshot_interval = 1000)`.
- Add `const Analytics& analytics() const { return analytics_; }` so the
  engine can read the final report after worker shutdown.
- `on_event()` calls `analytics_.on_event(ev)` on the worker's own copy.

**Changes to `engine.cpp`:**
- In `start_workers()`: construct `StatsWorker` without passing `analytics_`.
- In `print_summary()`: after `stop_workers()`, if threading was used, call
  `stats_worker_->analytics().print_report()` instead of
  `analytics_.print_report()`.

**Verify:** Core 0's `analytics_.on_event()` calls are already skipped when
threading is active (guarded by `if (!config_.is_threaded())`). Confirm this
by reading `engine.cpp`.

### MT-1b — Give RiskWorker its own shadow portfolio + analytics

The risk worker must track positions and analytics independently so it can
check limits without reading Core 0's mutable state.

**Changes to `threading/risk_worker.h`:**
- Replace `const portfolio& portfolio_` with `portfolio portfolio_` (owned).
- Replace `const Analytics& analytics_` with `Analytics analytics_` (owned).
- Constructor becomes:
  `RiskWorker(const RiskManager& rm, std::atomic<bool>& halt_flag)`.
  Remove the `portfolio&` and `Analytics&` parameters.
- In `on_event()`:
  - Feed ALL events to `analytics_.on_event(ev)` (so it tracks equity).
  - Feed fill events to `portfolio_.on_fill()`.
  - Then run risk checks against the worker's own `portfolio_` and
    `analytics_.snapshot()`.
- This means the risk worker's state **lags behind** Core 0 by ring buffer
  depth. This is acceptable: risk checks are advisory, the design doc
  explicitly permits this latency. A few extra orders may slip through before
  a halt arrives — that's the trade-off for not blocking the hot path.

**Changes to `engine.cpp` `start_workers()`:**
- Construct `RiskWorker(risk_manager_, halt_flag_)` — no portfolio/analytics
  refs.

### MT-1c — Remove inline risk checks when threading is active

In `engine::run()`, the `process_order` lambda runs risk checks inline AND
publishes to the risk ring. When a preset beyond inline_mode is active, the
inline checks are redundant.

**Changes:**
- Wrap pre-order and post-fill risk checks in `if (!config_.is_threaded())`.
- Core 0 already checks `halt_flag_` each iteration — the risk worker sets
  it asynchronously.

### MT-1d — ThreadSanitizer gate

Add a CMake option for TSan:

```cmake
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)
if(ENABLE_TSAN)
    target_compile_options(truetest PRIVATE -fsanitize=thread)
    target_link_options(truetest PRIVATE -fsanitize=thread)
    target_compile_options(truetest_tests PRIVATE -fsanitize=thread)
    target_link_options(truetest_tests PRIVATE -fsanitize=thread)
endif()
```

After this step, run the full test suite under TSan:
```bash
cmake -B build_tsan -DBUILD_TESTS=ON -DENABLE_TSAN=ON
cmake --build build_tsan
cd build_tsan && ctest --output-on-failure
```

All tests must pass with **zero TSan warnings**. If TSan reports races, fix
them before proceeding. Every subsequent step must also pass TSan.

### Tests
- Update `ThreadingEnabled_WorkersRun`: use `thread_preset::full`, verify
  `stats_worker->analytics()` has non-zero event counts.
- Add test: run a threaded backtest (with `--seed` for determinism), compare
  the stats worker's analytics report against a non-threaded baseline. Verify:
  same number of trades, same final equity (within float tolerance).
- Verify existing `HaltChannel_StopsEngine` and `GracefulShutdown_RingsDrained`
  still pass.
- Run TSan, confirm zero warnings.

---

## Step MT-2 — Composable Workers and Preset Wiring

**Goal:** Build the worker classes needed by each preset, then rewrite
`start_workers()` / `stop_workers()` to spawn only what the preset requires.

### MT-2a — Create ObserverWorker (for Light preset)

The Observer is a combined worker that runs logging, risk, and stats logic
on a single thread, consuming from a single ring.

**Create `threading/observer_worker.h`:**

```cpp
// Combined worker for the Light preset.
// Runs risk checking, stats accumulation, and logging on one thread.
// Consumes from a single shared ring buffer.
class ObserverWorker : public Worker
{
public:
    ObserverWorker(const RiskManager& rm,
                   std::atomic<bool>& halt_flag,
                   const std::string& event_log_path = "");

    void on_event(const event_pointer& ev) override;

    std::size_t events_processed() const;
    const Analytics& analytics() const;

private:
    // Risk checking (own shadow state)
    const RiskManager& risk_manager_;
    portfolio portfolio_;
    Analytics analytics_;
    std::atomic<bool>& halt_flag_;

    // Logging
    std::unique_ptr<EventLogger> event_logger_;  // optional binary log

    // Stats
    std::size_t snapshot_interval_ = 1000;
    AnalyticsReport last_snapshot_;

    std::atomic<std::size_t> events_processed_{0};
};
```

`on_event()` does all three jobs sequentially on each event:
1. `analytics_.on_event(ev)` — stats accumulation
2. Risk check (if order or fill event)
3. Logging (if event logger configured, write binary log; structured text
   log can also go here)

This is correct because all three operations are independent — they just
happen to share a thread.

### MT-2b — Create RiskStatsWorker (for Standard preset)

The Standard preset separates logging but keeps risk + stats together.

**Create `threading/risk_stats_worker.h`:**

```cpp
// Combined risk + stats worker for the Standard preset.
// Handles risk checking and analytics, but NOT logging (that gets its own thread).
class RiskStatsWorker : public Worker
{
public:
    RiskStatsWorker(const RiskManager& rm,
                    std::atomic<bool>& halt_flag);

    void on_event(const event_pointer& ev) override;

    std::size_t events_processed() const;
    const Analytics& analytics() const;

private:
    const RiskManager& risk_manager_;
    portfolio portfolio_;
    Analytics analytics_;
    std::atomic<bool>& halt_flag_;
    std::size_t snapshot_interval_ = 1000;
    AnalyticsReport last_snapshot_;
    std::atomic<std::size_t> events_processed_{0};
};
```

This is essentially the ObserverWorker minus logging.

### MT-2c — Rewrite start_workers() with preset dispatch

Replace the current `start_workers()` with a switch on the preset:

```cpp
void engine::start_workers()
{
    if (!config_.is_threaded())
        return;

    halt_flag_.store(false, std::memory_order_release);
    auto core_map = build_core_map();

    // Helper to find core_id for a role
    auto find_core = [&](core_role role) -> int {
        if (config_.disable_pinning) return -1;
        // check explicit overrides first (config_.pin_logging, etc.)
        // then fall back to core_map
        for (const auto& ca : core_map)
            if (ca.role == role) return ca.core_id;
        return -1;
    };

    switch (config_.threading)
    {
    case thread_preset::inline_mode:
        return;  // unreachable due to is_threaded() guard, but defensive

    case thread_preset::light:
    {
        // 1 ring, 1 ObserverWorker
        observer_ring_ = std::make_shared<EventRing>();
        observer_worker_ = std::make_unique<ObserverWorker>(
            risk_manager_, halt_flag_, config_.event_log_path);

        worker_threads_.emplace_back([this]() {
            observer_worker_->run(*observer_ring_);
        });
        pin_to_core(worker_threads_.back(), find_core(core_role::logging));
        break;
    }

    case thread_preset::standard:
    {
        // 2 rings: logging + risk/stats
        logging_ring_ = std::make_shared<EventRing>();
        risk_stats_ring_ = std::make_shared<EventRing>();

        logging_worker_ = std::make_unique<LoggingWorker>(
            config_.event_log_path);
        risk_stats_worker_ = std::make_unique<RiskStatsWorker>(
            risk_manager_, halt_flag_);

        worker_threads_.emplace_back([this]() {
            logging_worker_->run(*logging_ring_);
        });
        pin_to_core(worker_threads_.back(), find_core(core_role::logging));

        worker_threads_.emplace_back([this]() {
            risk_stats_worker_->run(*risk_stats_ring_);
        });
        pin_to_core(worker_threads_.back(), find_core(core_role::risk));
        break;
    }

    case thread_preset::full:
    {
        // 3 rings: logging + risk + stats (separate)
        logging_ring_ = std::make_shared<EventRing>();
        risk_ring_ = std::make_shared<EventRing>();
        stats_ring_ = std::make_shared<EventRing>();

        logging_worker_ = std::make_unique<LoggingWorker>(
            config_.event_log_path);
        risk_worker_ = std::make_unique<RiskWorker>(
            risk_manager_, halt_flag_);
        stats_worker_ = std::make_unique<StatsWorker>();

        // spawn + pin each
        // ... (same pattern as current code)
        break;
    }

    case thread_preset::extended:
    {
        // 4 outbound rings + 1 inbound (MM order ring)
        // Same as full, plus MarketMakerWorker
        logging_ring_ = std::make_shared<EventRing>();
        risk_ring_ = std::make_shared<EventRing>();
        stats_ring_ = std::make_shared<EventRing>();
        mm_ring_ = std::make_shared<EventRing>();        // outbound to MM
        mm_order_ring_ = std::make_shared<EventRing>();  // inbound from MM

        // ... create all 4 workers, spawn + pin
        break;
    }
    }
}
```

### MT-2d — Rewrite publish_event() with preset dispatch

`publish_event()` pushes to different rings depending on the preset:

```cpp
void engine::publish_event(const event_pointer& ev)
{
    switch (config_.threading)
    {
    case thread_preset::inline_mode:
        return;

    case thread_preset::light:
        if (observer_ring_) observer_ring_->try_push(ev);
        return;

    case thread_preset::standard:
        if (logging_ring_) logging_ring_->try_push(ev);
        if (risk_stats_ring_) risk_stats_ring_->try_push(ev);
        return;

    case thread_preset::full:
        if (logging_ring_) logging_ring_->try_push(ev);
        if (risk_ring_) risk_ring_->try_push(ev);
        if (stats_ring_) stats_ring_->try_push(ev);
        return;

    case thread_preset::extended:
        if (logging_ring_) logging_ring_->try_push(ev);
        if (risk_ring_) risk_ring_->try_push(ev);
        if (stats_ring_) stats_ring_->try_push(ev);
        if (mm_ring_) mm_ring_->try_push(ev);
        return;
    }
}
```

### MT-2e — Update engine.h with new member variables

Add the new ring and worker pointers:

```cpp
// Preset-specific rings and workers (only the ones used by the active
// preset are non-null)
std::shared_ptr<EventRing> observer_ring_;     // light
std::shared_ptr<EventRing> risk_stats_ring_;   // standard
std::shared_ptr<EventRing> mm_ring_;           // extended (outbound)
std::shared_ptr<EventRing> mm_order_ring_;     // extended (inbound from MM)

std::unique_ptr<ObserverWorker> observer_worker_;
std::unique_ptr<RiskStatsWorker> risk_stats_worker_;
// existing: logging_worker_, risk_worker_, stats_worker_
std::unique_ptr<MarketMakerWorker> mm_worker_;  // created in MT-3
```

### MT-2f — Update print_summary() to use correct analytics source

After the run, the engine needs the analytics from whichever worker
accumulated stats:

```cpp
void engine::print_summary()
{
    switch (config_.threading)
    {
    case thread_preset::inline_mode:
        analytics_.print_report();
        return;
    case thread_preset::light:
        if (observer_worker_)
            observer_worker_->analytics().print_report();
        return;
    case thread_preset::standard:
        if (risk_stats_worker_)
            risk_stats_worker_->analytics().print_report();
        return;
    case thread_preset::full:
    case thread_preset::extended:
        if (stats_worker_)
            stats_worker_->analytics().print_report();
        return;
    }
}
```

### Tests
- Test each preset independently:
  - `inline_mode`: verify no rings created, no threads spawned.
  - `light`: 1 ring, observer_worker processes events.
  - `standard`: 2 rings, logging + risk_stats workers process events.
  - `full`: 3 rings, each worker processes events independently.
- Run a deterministic backtest (`--seed 42`) at each preset. Verify trade
  count matches inline_mode (may differ by 0-1 due to ring latency in risk
  halt edge cases — document the tolerance).
- Run TSan on all preset tests.

---

## Step MT-3 — Market Maker Worker (Extended preset)

**Problem:** The market maker currently runs inline on Core 0
(`market_maker_.replenish()` inside the bar loop). The Extended preset moves
it to its own thread.

### MT-3a — Create MarketMakerWorker

**Create `threading/market_maker_worker.h`:**

The market maker worker does NOT touch the orderbook directly. Instead, it
produces `order_event`s and pushes them into an inbound ring that Core 0
drains. This keeps the orderbook single-writer (Core 0 only).

```cpp
class MarketMakerWorker : public Worker
{
public:
    MarketMakerWorker(unsigned seed,
                      RingBuffer<event_pointer, N>& order_ring);

    void on_event(const event_pointer& ev) override;

    std::size_t events_processed() const;
    std::size_t orders_generated() const;

private:
    MarketMaker mm_;                // owned, seeded independently
    EventRing& order_ring_;         // inbound ring back to Core 0
    ObjectPool<order_event> pool_;  // pool for generated orders

    std::atomic<std::size_t> events_processed_{0};
    std::atomic<std::size_t> orders_generated_{0};
};
```

`on_event()` logic:
- Only cares about `market_event` (ignore other types).
- On market_event: compute the replenish orders the market maker would
  normally place. Instead of calling `orderbook::add_order()`, construct
  `order_event`s and push them into `order_ring_`.
- The market maker's volatility tracking and spread calculation happen
  entirely within its own thread.

### MT-3b — Refactor MarketMaker to produce order_events

Currently `MarketMaker::replenish()` takes a `shared_ptr<orderbook>` and
calls `ob->add_order()` directly. Refactor it to return a vector of orders
instead:

```cpp
struct mm_order
{
    order_side side;
    double price;
    int quantity;
};

// Returns the orders the market maker wants to place.
// Caller decides whether to add them to the book directly (inline mode)
// or push them into a ring (extended mode).
std::vector<mm_order> MarketMaker::compute_replenish(double current_price);
```

The existing `replenish()` method calls `compute_replenish()` internally and
applies the orders to the book — backward compatible for all presets except
extended.

### MT-3c — Core 0 drains MM inbound ring

In `engine::run()`, when preset is `extended`, add a drain step at the top
of each iteration (alongside pending_orders):

```cpp
// Drain market maker orders (extended preset only)
if (mm_order_ring_)
{
    event_pointer mm_ev;
    while (mm_order_ring_->try_pop(mm_ev))
    {
        if (mm_ev->get_type() == event_type::order)
        {
            auto& mm_order = static_cast<order_event&>(*mm_ev);
            auto adapter = get_adapter(mm_order.get_symbol());
            adapter->submit_order(mm_order);
            // poll fills ...
        }
    }
}
```

### MT-3d — Skip inline replenish when extended

In `engine::run()`, wrap `market_maker_.replenish()` in:
```cpp
if (!preset_has_mm_worker(config_.threading))
    market_maker_.replenish(ob, last_mid_price_);
```

### Tests
- Feed market events into MM worker, verify orders appear on inbound ring.
- Run extended preset backtest, verify trades still occur (MM provides
  liquidity via ring instead of inline).
- Compare trade count between full and extended presets (should be similar
  but not necessarily identical due to timing differences).
- TSan clean.

---

## Step MT-4 — Real Logging Worker

**Problem:** `LoggingWorker::on_event()` is a no-op stub. It counts events
but doesn't write anything. Also, the `EventLogger` (binary event log from
Step 10) runs inline on Core 0 — this puts file I/O on the hot path.

### MT-4a — Implement batched structured logging

**Changes to `threading/logging_worker.h`:**

```cpp
class LoggingWorker : public Worker
{
public:
    enum class log_sink { none, stdout_sink, file_sink };

    explicit LoggingWorker(const std::string& event_log_path = "",
                           log_sink text_sink = log_sink::none,
                           const std::string& text_log_path = "");
    ~LoggingWorker();  // flushes remaining buffer

    void on_event(const event_pointer& ev) override;
    std::size_t events_processed() const;

private:
    // Binary event log (from Step 10)
    std::unique_ptr<EventLogger> event_logger_;

    // Structured text log
    log_sink text_sink_;
    std::ofstream text_file_;
    std::ostringstream batch_buffer_;
    std::size_t batch_count_ = 0;
    static constexpr std::size_t BATCH_SIZE = 100;

    std::atomic<std::size_t> events_processed_{0};

    void flush_batch();
    std::string format_event(const event& ev) const;
};
```

`on_event()` does:
1. If `event_logger_` exists: `event_logger_->log(*ev)`.
2. If `text_sink_` is not none: format + batch + flush when full.

**Why batching:** Each unbatched write is a syscall. At 100k events/sec,
that's 100k syscalls. Batching to 100 events → 1k syscalls, negligible.

### MT-4b — Move EventLogger off Core 0

When any threaded preset is active:
- Do NOT create `EventLogger` inline in `engine::run()`.
- Instead, pass `event_log_path` to the LoggingWorker (or ObserverWorker
  in Light preset). The worker creates the `EventLogger` internally.
- Remove the inline `log_event()` calls from `engine::run()` when threaded.
  Keep them for `inline_mode`.

### MT-4c — Add log config to engine_config

Add fields for text logging:

```cpp
struct engine_config
{
    // ... existing ...
    std::string text_log_path;   // empty = no text log
    bool log_to_stdout = false;  // text log to stdout (for debugging)
};
```

### Tests
- File sink: feed events, verify file has content after worker stops.
- Batch flush: feed fewer than BATCH_SIZE events, stop worker, verify
  incomplete batch was flushed.
- Binary event log: verify `EventLogger` produces valid log when run via
  logging worker thread (round-trip test).
- Event count matches input.

---

## Step MT-5 — Hot Path Audit & Optimisation

**Goal:** Verify Core 0 does the absolute minimum when threading is active.
This is a review + cleanup step, not a feature step.

### MT-5a — Audit engine::run() line by line

When `config_.is_threaded()` is true, the bar loop body should contain ONLY:

1. Construct `market_event` from data handler vectors (stack, no heap).
2. Drain inbound rings (`mm_order_ring_` in extended, `pending_orders`).
3. `strategy_->on_market(mkt)` — user strategy code.
4. `orderbook::add_order()` / matching → fills.
5. `portfolio_.on_fill()` — position tracking.
6. `publish_event()` — N `try_push()` calls (non-blocking, no syscall).
7. Check `halt_flag_` — single relaxed atomic load.

**Verify removal of (when threaded):**
- `analytics_.on_event()` — guarded by `if (!config_.is_threaded())`.
- `risk_manager_.check_order()` / `check_post_fill()` — moved in MT-1c.
- `market_maker_.replenish()` — skipped in extended (MT-3d), inline otherwise.
- `event_logger_->log()` — moved to logging worker (MT-4b).
- `std::cout` progress — see MT-5b.

If any of these still run on Core 0 when threaded, fix them.

### MT-5b — Throttle progress reporting

Replace per-bar `std::cout << "\rProgress: ..."` with time-based throttling:

```cpp
auto last_report = std::chrono::steady_clock::now();
// ...
auto now = std::chrono::steady_clock::now();
if (now - last_report >= std::chrono::milliseconds(200))
{
    std::cout << "\rProgress: " << ...;
    last_report = now;
}
```

This eliminates ~10 formatting + I/O calls per second (currently one per
`report_interval` bars).

### MT-5c — Apply same audit to run_tick_data()

Mirror the bar loop audit for the tick loop. Same rules: when threaded,
Core 0 only does tick → strategy → orderbook → portfolio → publish.

### Tests
- Run a large backtest (100k bars) with each preset. Measure throughput.
  Threading should not be slower than inline_mode (modulo thread startup).
- Run TSan, confirm still clean.

---

## Step MT-6 — Graceful Degradation & Error Handling

**Problem:** If a worker thread throws (disk full, corrupted event), the
engine doesn't know. The thread silently dies, its ring fills up, events
are silently dropped.

### MT-6a — Worker exception capture

**Changes to `threading/worker.h`:**

Wrap the `run()` loop in try-catch:

```cpp
template <std::size_t N, typename Policy>
void run(RingBuffer<event_pointer, N, Policy>& inbound)
{
    running_.store(true, std::memory_order_release);
    try
    {
        event_pointer ev;
        while (running_.load(std::memory_order_acquire))
        {
            if (inbound.try_pop(ev))
                on_event(ev);
        }
        while (inbound.try_pop(ev))
            on_event(ev);
    }
    catch (...)
    {
        exception_ = std::current_exception();
        running_.store(false, std::memory_order_release);
    }
}
```

Add to Worker:
- `std::exception_ptr exception_` member.
- `std::exception_ptr get_exception() const` getter.
- `bool has_failed() const` — returns true if exception is stored.

### MT-6b — Shared failure flag

Add `std::atomic<bool> worker_failed_{false}` to the engine. Pass a
reference to each worker. The worker sets it to true if it catches an
exception.

In the engine loop, check alongside `halt_flag_`:

```cpp
if (halt_flag_.load(std::memory_order_acquire) ||
    worker_failed_.load(std::memory_order_acquire))
    break;
```

In `stop_workers()`, after joining all threads, check each worker's
exception and print/rethrow:

```cpp
for (auto* w : all_workers)
{
    if (auto ex = w->get_exception())
    {
        try { std::rethrow_exception(ex); }
        catch (const std::exception& e) {
            std::cerr << "Worker failed: " << e.what() << "\n";
        }
    }
}
```

### MT-6c — Ring drop counters

Add a drop counter that increments when `try_push()` returns false:

```cpp
void engine::publish_event(const event_pointer& ev)
{
    // ... for each ring:
    if (some_ring_ && !some_ring_->try_push(ev))
        some_ring_drops_++;
}
```

Report in `print_summary()`:
```
Ring buffer drops: logging=0, risk=0, stats=0, mm=0
```

If drops > 0, print a warning:
```
WARNING: N events were dropped. Consider increasing ring_buffer_capacity.
```

### Tests
- Create a worker subclass that throws after 5 events. Run a threaded
  backtest, verify the engine stops and reports the exception text.
- Create a test with a tiny ring (capacity 8). Verify drops are counted,
  engine still completes, summary reports drops.
- TSan clean.

---

## Step MT-7 — Benchmarking & Correctness Validation

**Goal:** Prove that threading helps and doesn't break anything.

### MT-7a — Benchmark mode

Add `--benchmark` CLI flag:

1. Auto-detect preset.
2. Run the backtest in `inline_mode` — record wall time, throughput, trades.
3. Run the same backtest at the detected preset — record same metrics.
4. Print comparison:

```
Benchmark Results (100,000 bars, seed=42)
─────────────────────────────────────────────────
Preset          │ Time (ms) │ Events/sec │ Trades
────────────────┼───────────┼────────────┼───────
inline          │       450 │  2,222,222 │     47
light           │       380 │  2,631,578 │     47
standard        │       340 │  2,941,176 │     47
full            │       320 │  3,125,000 │     47
extended        │       310 │  3,225,806 │     47
─────────────────────────────────────────────────
```

Use `--seed` for determinism. Run each preset, not just inline vs. detected.

### MT-7b — Correctness matrix

For each preset pair, verify that key metrics match within tolerance:

```cpp
// After running at each preset, compare:
EXPECT_EQ(results[inline_mode].trades, results[full].trades);
EXPECT_NEAR(results[inline_mode].final_equity,
            results[full].final_equity, 0.01);
EXPECT_NEAR(results[inline_mode].max_drawdown,
            results[full].max_drawdown, 0.01);
```

**Caveat:** When risk checks run on a worker thread (all presets except
inline_mode), the risk halt arrives a few events late. For the correctness
matrix, use permissive risk limits (no halts triggered) so the hot path
is identical across presets.

### MT-7c — Add to CI

The TSan build from MT-1d should already run in CI. Add the correctness
matrix test to the standard test suite. The benchmark is optional (not a
CI gate — it's hardware-dependent).

### Tests
- `tests/test_threading_benchmark.cpp` (optional, not CI):
  measure throughput per preset for 100k bars.
- `tests/test_threading_correctness.cpp` (CI gate):
  run the same deterministic backtest at all 5 presets, verify trade count
  and final equity match.

---

## Summary

| Step   | What                              | Key Changes                                       | Why                           |
|--------|-----------------------------------|---------------------------------------------------|-------------------------------|
| MT-0   | Preset enum & config              | Replace bool with `thread_preset`, auto-detection  | Foundation for scaling         |
| MT-1   | Fix data races                    | Workers own their state, TSan gate                 | Correctness (UB elimination)   |
| MT-2   | Composable workers + wiring       | ObserverWorker, RiskStatsWorker, preset dispatch   | Adaptive to hardware           |
| MT-3   | Market maker worker               | MarketMakerWorker, inbound order ring              | Hot path decontamination       |
| MT-4   | Real logging worker               | Batched I/O, EventLogger offload                   | I/O off hot path               |
| MT-5   | Hot path audit                    | Verify Core 0 is minimal when threaded             | Latency reduction              |
| MT-6   | Error handling                    | Exception capture, ring drop counters              | Robustness                     |
| MT-7   | Benchmarking & validation         | Per-preset benchmarks, correctness matrix, TSan CI | Confidence                     |

## Shared State Reference (after all steps complete)

| State                     | Owner (writer)     | Readers             | Sync mechanism                |
|---------------------------|--------------------|----------------------|-------------------------------|
| `data_handler_`           | nobody (read-only) | Core 0               | immutable after load          |
| `orderbook_registry_`     | Core 0             | Core 0 only          | single-writer                 |
| `portfolio_`              | Core 0             | Core 0 only          | single-writer                 |
| `analytics_` (engine's)   | Core 0             | Core 0 only          | single-writer (inline only)   |
| `strategy_`               | Core 0             | Core 0 only          | single-writer                 |
| `risk_manager_`           | nobody (immutable) | Core 0, risk worker  | immutable after construction  |
| `halt_flag_`              | risk worker        | Core 0               | `std::atomic<bool>`           |
| `worker_failed_`          | any worker         | Core 0               | `std::atomic<bool>`           |
| observer_ring_            | Core 0 (push)      | observer worker (pop)| SPSC ring buffer              |
| logging_ring_             | Core 0 (push)      | logging worker (pop) | SPSC ring buffer              |
| risk_ring_                | Core 0 (push)      | risk worker (pop)    | SPSC ring buffer              |
| risk_stats_ring_          | Core 0 (push)      | risk+stats (pop)     | SPSC ring buffer              |
| stats_ring_               | Core 0 (push)      | stats worker (pop)   | SPSC ring buffer              |
| mm_ring_                  | Core 0 (push)      | MM worker (pop)      | SPSC ring buffer              |
| mm_order_ring_            | MM worker (push)   | Core 0 (pop)         | SPSC ring buffer              |
| Observer's portfolio      | observer worker    | observer only        | thread-local                  |
| Observer's analytics      | observer worker    | observer only        | thread-local                  |
| RiskStats' portfolio      | risk_stats worker  | risk_stats only      | thread-local                  |
| RiskStats' analytics      | risk_stats worker  | risk_stats only      | thread-local                  |
| Risk worker's portfolio   | risk worker        | risk worker only     | thread-local                  |
| Risk worker's analytics   | risk worker        | risk worker only     | thread-local                  |
| Stats worker's analytics  | stats worker       | stats worker only    | thread-local                  |
| MM worker's MarketMaker   | MM worker          | MM worker only       | thread-local                  |

**Rule:** If two threads touch the same data, it must be through a ring buffer
or an atomic. No mutexes on the hot path. No shared mutable references.
Each preset uses only the rings and workers it needs — no unused rings are
allocated.

## Preset Quick Reference

| Preset    | Cores | Threads | Rings | Workers spawned                          |
|-----------|-------|---------|-------|------------------------------------------|
| inline    | 1-2   | 1       | 0     | none                                     |
| light     | 3     | 2       | 1     | ObserverWorker                           |
| standard  | 4-5   | 3       | 2     | LoggingWorker + RiskStatsWorker          |
| full      | 6-7   | 4       | 3     | LoggingWorker + RiskWorker + StatsWorker |
| extended  | 8+    | 5       | 4+1   | Logging + Risk + Stats + MarketMaker     |
