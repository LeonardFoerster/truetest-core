# Performance roadmap

This document is an actionable, prioritised list of performance work for the
TrueTest engine. Items are sorted by **impact-vs-risk**, not by ease. Each
item explains *why* it makes the engine faster and *why it does not change
observable behaviour* (semantics, outputs, fill prices, strategy decisions),
followed by step-by-step instructions for Claude Code to execute.

> **Read this first.** Optimisation without measurement is gambling. Step 1
> in the High tier is *measurement*, not a code change. Do not skip it.
> Every subsequent step should be re-evaluated against the latest profiler
> output — this list is a starting hypothesis, not a contract.

After each step, run the full test suite (`./build/truetest_tests`) and the
three smoke binaries (`engine_backtest`, `engine_shadow`, `engine_live` with
`--no-tui`). The single pre-existing failure `DataHandler.HasBarData` is
unrelated; everything else must stay green.

---

## High impact

These deliver the largest expected return and have the lowest risk of
behavioural drift. Do them in order.

### 1. Capture a `StageTimer` baseline

**Why this helps performance.** It doesn't, directly. It tells you *where*
the time is actually being spent so the rest of this list is grounded in
reality instead of speculation.

**Why behaviour is unchanged.** Pure measurement build. No code edits, only
a different CMake flag. The `HAS_DEBUG` build path is already supported
and unrelated to trading semantics.

**Steps**

1. Configure a debug-instrumented build:
   ```bash
   cmake -B build-perf -DENABLE_DEBUG=ON -DENABLE_QUESTDB=ON \
                       -DENABLE_BINANCE=ON -DENABLE_NATIVE_OPT=ON \
                       -DCMAKE_BUILD_TYPE=Release
   cmake --build build-perf -j --target engine_live engine_shadow
   ```
2. Run a representative workload:
   - For `engine_shadow`: a recorded Binance WS replay long enough to cover
     1–5 minutes of live data (use `BinanceRecorder` to capture; replay via
     `BinanceReplayTransport`).
   - For `engine_live`: a paper-mode Binance live stream for 5+ minutes.
3. Capture the `StageTimer` output and ring HWMs printed at end-of-run into
   `docs/perf-baseline.md`. Include: build SHA, workload description, and
   raw stage breakdown.
4. Optionally also run under `perf record -g -- ./build-perf/engine_shadow …`
   and capture the top-of-stack flame graph for cross-reference.
5. **Do not proceed past this step until the baseline is captured.** Every
   subsequent optimisation is judged against this number.

**Verification.** None — this is the verification baseline itself.

---

### 2. Swap the allocator to mimalloc (or jemalloc)

**Why this helps performance.** Glibc's `malloc` is general-purpose and pays
for it on small short-lived allocations. Hot paths in this engine allocate
constantly: `std::string symbol` inside every event, lots/orders maps,
ring elements. mimalloc/jemalloc typically cut allocation latency by
40–70 % and reduce fragmentation. Wins compound across the whole binary.

**Why behaviour is unchanged.** A drop-in `malloc` replacement honours the
same C/C++ allocation contract. Outputs, fills, decisions all identical —
only the time spent inside `operator new` / `malloc` changes.

**Steps**

1. Add a CMake option in `CMakeLists.txt`:
   ```cmake
   option(ENABLE_MIMALLOC "Override the system allocator with mimalloc" OFF)
   ```
2. In `cmake/Dependencies.cmake`, inside `tt_fetch_dependencies()`, add:
   ```cmake
   if(ENABLE_MIMALLOC)
       FetchContent_Declare(
           mimalloc
           GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
           GIT_TAG        v2.1.7
       )
       set(MI_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
       set(MI_BUILD_OBJECT   ON  CACHE BOOL "" FORCE)
       set(MI_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
       set(MI_OVERRIDE       ON  CACHE BOOL "" FORCE)
       FetchContent_MakeAvailable(mimalloc)
   endif()
   ```
3. In `CMakeLists.txt`, after `_tt_add_engine_binary` calls, link
   `mimalloc-static` to each engine binary (not `engine_core` — keep the
   OBJECT library allocator-neutral so tests are unaffected):
   ```cmake
   if(ENABLE_MIMALLOC)
       foreach(t IN ITEMS engine_backtest engine_shadow engine_live)
           target_link_libraries(${t} PRIVATE mimalloc-static)
       endforeach()
   endif()
   ```
4. Configure and build with `-DENABLE_MIMALLOC=ON`.
5. Verify the linker pulled it in: `nm build/engine_live | grep mi_malloc`
   should return symbols.
6. Re-run the StageTimer workload from step 1 and diff against baseline.

**Verification.** Full test suite passes. StageTimer shows reduced time in
allocation-heavy stages.

---

### 3. Enable Profile-Guided Optimisation (PGO)

**Why this helps performance.** PGO uses runtime profile data to make better
inlining, branch-prediction, and basic-block layout decisions than the
compiler can guess statically. For HFT-shaped code with predictable
branches (e.g. "fill rarely arrives", "risk check rarely rejects"), wins
are routinely 10–20 % on the hot path. Free at runtime.

**Why behaviour is unchanged.** PGO only reorders code and changes inlining
decisions. It does not modify program semantics — same outputs, same
floating-point results (with default flags), same observable behaviour.

**Steps**

1. Add two CMake options to `CMakeLists.txt`:
   ```cmake
   option(ENABLE_PGO_GENERATE "Build with -fprofile-generate (training stage)" OFF)
   option(ENABLE_PGO_USE      "Build with -fprofile-use (final stage)"          OFF)
   ```
2. In `cmake/CompilerFlags.cmake`, in `tt_apply_common_flags(target)`, append:
   ```cmake
   if(ENABLE_PGO_GENERATE)
       target_compile_options(${target} PRIVATE -fprofile-generate=${CMAKE_BINARY_DIR}/pgo-data)
       target_link_options(${target}    PRIVATE -fprofile-generate=${CMAKE_BINARY_DIR}/pgo-data)
   endif()
   if(ENABLE_PGO_USE)
       target_compile_options(${target} PRIVATE -fprofile-use=${CMAKE_BINARY_DIR}/pgo-data
                                                -fprofile-correction)
       target_link_options(${target}    PRIVATE -fprofile-use=${CMAKE_BINARY_DIR}/pgo-data)
   endif()
   ```
3. Document the workflow in this file's appendix (also add to README/CLAUDE.md):
   ```bash
   # Stage 1: instrumented build
   cmake -B build-pgo -DENABLE_PGO_GENERATE=ON -DENABLE_NATIVE_OPT=ON \
                      -DCMAKE_BUILD_TYPE=Release
   cmake --build build-pgo -j --target engine_live

   # Stage 2: training run (use a representative workload)
   ./build-pgo/engine_live --provider binance --symbol btcusdt \
       --stream kline_1m --strategy sma   # let it run 5–10 minutes

   # Stage 3: optimised rebuild
   cmake -B build-pgo -DENABLE_PGO_GENERATE=OFF -DENABLE_PGO_USE=ON
   cmake --build build-pgo -j --target engine_live
   ```
4. Add a CI guard so PGO is opt-in: never enable in default CI builds.

**Verification.** Final binary runs identically; benchmark hot path against
non-PGO baseline.

---

### 4. Audit `log_event` for hot-path text formatting

**Why this helps performance.** `LoggingWorker` drains its ring asynchronously,
but if `log_event(*o)` *formats* the text on the event-loop thread before
pushing the formatted string to the ring, the formatting cost lives on the
hot path. Moving formatting into the worker shifts that cost to a thread
that has time to spare.

**Why behaviour is unchanged.** Logs land at the same sinks with the same
content. The only difference is *which thread* assembled the formatted
string. Order of log lines vs trade events is preserved by the same ring.

**Steps**

1. Read `src/engine/logging_worker.h` end-to-end. Note where formatting
   happens — at `push` time (bad) or in the worker's drain loop (good).
2. Search for every `log_event` call in `src/engine/engine.cpp`:
   ```bash
   grep -n "log_event\|logging_worker_->\|push(.*log" src/engine/engine.cpp
   ```
3. For each call, trace whether the value pushed onto the ring is the raw
   event (cheap) or a pre-formatted `std::string` (expensive).
4. If pre-formatted: change the ring element type to `event_pointer` and
   move the snprintf/string-building into `LoggingWorker::on_event`.
5. Take care that any sink that buffers (e.g. file sink) is still safe with
   the new layout.

**Verification.** Test suite passes. StageTimer should show reduced time in
the "log" stage on the event loop and (correspondingly) more activity in
the logging worker thread.

---

### 5. Replace hot `std::unordered_map` with `absl::flat_hash_map`

**Why this helps performance.** `std::unordered_map` is a chained hash table
with a node-per-element allocation. Open-addressing hash tables (Abseil's
Swiss tables, Boost's `unordered_flat_map`) are 2–5× faster on small to
medium N because they avoid the per-element heap allocation, have better
cache locality, and use SIMD probing. Identical API, drop-in replacement.

**Why behaviour is unchanged.** The container is unordered in both
implementations; iteration order differences are not observable through
the engine's public API (no test asserts iteration order). Lookups,
inserts, and erases produce the same logical result.

**Steps**

1. Make Abseil unconditionally available. In `cmake/Dependencies.cmake`,
   move the `FetchContent_Declare(abseil-cpp ...)` block out of the
   `if(ENABLE_DEBUG)` branch in `tt_wire_optional_backends` and into the
   always-fetched `tt_fetch_dependencies()` function. Drop the `if(NOT TARGET absl::log)` guard.
2. In `engine_core`'s link list (`CMakeLists.txt`), add
   `absl::flat_hash_map`.
3. Replace each of the following declarations:
   - `src/execution/portfolio.h`:
     ```cpp
     std::unordered_map<std::string, position> positions_;
     std::unordered_map<std::uint64_t, lot>    lots_;
     ```
     → `absl::flat_hash_map<…>`
   - `src/execution/order_tracker.h`: `std::unordered_map<uint64_t, order_status> statuses_;`
   - `src/engine/engine.h`:
     - `std::unordered_map<uint64_t, order_meta> order_meta_;`
     - `std::unordered_map<std::uint64_t, open_order_cache_entry> open_orders_cache_;`
     - `std::unordered_map<std::string, std::shared_ptr<IExecutionAdapter>> execution_adapters_;`
4. Add `#include <absl/container/flat_hash_map.h>` to each touched header.
5. Run `truetest_tests`. If any test asserts iteration order, file a follow-up
   issue (the test was relying on undefined behaviour anyway).

**Verification.** Test suite passes. StageTimer "fill processing" and
"order routing" stages should drop measurably.

---

### 6. Symbol interning (`std::string symbol` → `uint32_t symbol_id`)

**Why this helps performance.** Every `order_event`, `fill_event`,
`market_event`, etc. carries a `std::string` symbol. Each construction
allocates (unless SSO covers it — and for symbols like `BTCUSDT`, exactly
on the SSO boundary). Hashing strings is also slower than hashing a
`uint32_t`. Interning collapses this to a 4-byte handle plus a single
global string table looked up only when the symbol must be displayed.

**Why behaviour is unchanged.** The ID ↔ string mapping is bijective and
stable for the run. Anywhere the engine compares symbols (orderbook
lookup, position keying), comparing IDs is semantically identical to
comparing strings. Outputs are converted back to strings only at the
display/log/persistence boundary.

**Caution.** This is the largest refactor in the High tier. Only
attempt after the StageTimer baseline shows symbol-string allocation as a
real cost. Touches event types, every consumer, every test. Estimate
multiple days of work plus careful review.

**Steps**

1. Create `src/types/symbol.h`:
   ```cpp
   namespace truetest {
       using symbol_id = std::uint32_t;
       constexpr symbol_id kInvalidSymbol = 0;

       class SymbolTable {
       public:
           symbol_id intern(std::string_view s);     // thread-safe
           const std::string& name(symbol_id id) const;
       private:
           mutable std::shared_mutex mu_;
           std::vector<std::string> by_id_;
           absl::flat_hash_map<std::string, symbol_id> by_name_;
       };
       SymbolTable& global_symbols();
   }
   ```
2. Add the corresponding `src/types/symbol.cpp` and wire into `engine_core`.
3. In `src/core/event.h`, replace `std::string symbol_` with
   `truetest::symbol_id symbol_id_` on every event type. Provide a
   transitional `get_symbol()` that returns `std::string_view`
   resolving via the table.
4. Update every event constructor call in providers (parsers) to call
   `global_symbols().intern(...)` once at the parser edge.
5. Update `portfolio_.positions_` keying: was `std::string`, becomes
   `symbol_id`. Same for `engine.execution_adapters_`,
   `orderbook_registry_`, `last_mark_symbol_`, etc.
6. Update display sites (dashboard panels, logging, QuestDB store) to
   resolve the ID via `global_symbols().name()` only at write time.
7. Update tests to use the new API.

**Verification.** Full test suite passes. StageTimer shows reduced
allocator activity per event. Heap profile (e.g. `heaptrack`) confirms
fewer short-lived allocations.

---

## Medium impact

Smaller wins, contained scope. Each item is a couple of hours.

### 7. Convert `recent_fills_cache_` from `std::deque` to a ring buffer

**Why this helps performance.** `std::deque` allocates 4 KB chunks lazily.
The cache is bounded at 64 entries — an array is strictly cheaper:
zero allocations after construction, perfect cache locality.

**Why behaviour is unchanged.** Same FIFO semantics: oldest gets evicted
once the cap is reached. The dashboard reads from newest to oldest in
both implementations.

**Steps**

1. In `src/engine/engine.h`, replace
   `std::deque<truetest::ui::dashboard_snapshot::fill_row> recent_fills_cache_;`
   with a fixed circular array:
   ```cpp
   std::array<truetest::ui::dashboard_snapshot::fill_row, kRecentFillsCap>
       recent_fills_ring_{};
   std::size_t recent_fills_head_ = 0;   // next write slot
   std::size_t recent_fills_size_ = 0;   // 0..kRecentFillsCap
   ```
2. In `src/engine/engine.cpp` `cache_fill`, replace the push-front /
   pop-back logic with:
   ```cpp
   recent_fills_ring_[recent_fills_head_] = std::move(r);
   recent_fills_head_ = (recent_fills_head_ + 1) % kRecentFillsCap;
   if (recent_fills_size_ < kRecentFillsCap) ++recent_fills_size_;
   ```
3. In `build_dashboard_view`, copy newest-first by walking backwards from
   `head` for `size` slots.
4. Add `<array>` include.

**Verification.** Test suite passes. Smoke runs unchanged.

---

### 8. Convert `AdverseSelectionTracker::pending_` from `std::deque` to a ring buffer

**Why this helps performance.** Same argument as above, at higher scale:
`max_pending` defaults to 16 384 — that's hundreds of allocator calls per
session. A fixed-size ring (sized from `cfg.max_pending` at construction)
removes them.

**Why behaviour is unchanged.** Tracker semantics are FIFO with capacity
bound; the existing implementation drops oldest on overflow. A ring with
the same overflow rule produces the same `mean_bps()`, `stdev_bps()`,
`sample_count()`, `dropped_count()`.

**Steps**

1. In `src/analytics/adverse_selection_tracker.h`, replace
   `std::deque<pending_fill> pending_` with a fixed-capacity ring sized
   from `cfg_.max_pending`. Track head/tail indices.
2. In `adverse_selection_tracker.cpp`, update `on_fill` (push to head,
   bump dropped on overflow) and `on_mark` (drain from tail when due).
3. Verify the existing tests in `tests/test_adverse_selection_tracker.cpp`
   still pass.

**Verification.** Tests pass. StageTimer shows reduced allocator activity
in fill-processing path.

---

### 9. Pre-allocate snapshot vectors in `engine::build_dashboard_view`

**Why this helps performance.** `build_dashboard_view` calls `clear()` +
`reserve()` on the snapshot's vectors every refresh. `reserve()` only
prevents reallocation if the cap was high enough — for the first refresh
each tick it pays for the heap allocation. Pre-sizing once at engine
construction (or reusing buffers) eliminates allocator calls on the
event-loop thread entirely.

**Why behaviour is unchanged.** The data going into the snapshot is
unchanged. Only the path the bytes take through the allocator differs.

**Steps**

1. Add member buffers to the engine (private):
   ```cpp
   mutable std::vector<truetest::ui::dashboard_snapshot::position_row>   pos_scratch_;
   mutable std::vector<truetest::ui::dashboard_snapshot::lot_row>        lot_scratch_;
   mutable std::vector<truetest::ui::dashboard_snapshot::open_order_row> ord_scratch_;
   mutable std::vector<truetest::ui::dashboard_snapshot::fill_row>       fill_scratch_;
   ```
2. In the constructor, reserve sane upper bounds (e.g. 32, 64, 128, 64).
3. In `build_dashboard_view`, fill the scratch buffers, then `out.positions = pos_scratch_;`
   etc. — or better, swap. Avoid reallocating on every call.

**Verification.** Test suite passes. Allocator profile in heaptrack should
show no per-tick allocations from the dashboard refresh path.

---

### 10. Lock-free `dashboard_view_` (triple buffer or atomic shared_ptr)

**Why this helps performance.** The current `dashboard_view_mu_` is held
microseconds at most, but it's still a real lock on the event loop every
100 ms. Triple buffering removes it: writers fill an inactive slot,
atomically publish the index, readers atomically load the index. Wait-free
on both sides.

**Why behaviour is unchanged.** The snapshot the reader observes is still
*some* coherent snapshot — older or newer, but never torn. Same as today.

**Steps**

1. Replace the mutex + slot pair with three pre-allocated `dashboard_snapshot`s:
   ```cpp
   std::array<truetest::ui::dashboard_snapshot, 3> view_slots_;
   std::atomic<std::uint8_t> view_active_{0};   // index of latest published
   ```
2. In `refresh_dashboard_view_if_due`, pick a non-active slot, fill it,
   `view_active_.store(idx, std::memory_order_release)`.
3. In `snapshot_dashboard`, atomic load the index and copy the snapshot
   from that slot. The race window (writer reuses the slot the reader is
   reading) is bounded if you maintain *three* slots: writer never picks
   the currently active one.
4. Remove `dashboard_view_mu_` and the `dashboard_view_initialised_`
   bool — replace with `view_active_.load() == kInvalid` semantics
   (initialise with `kInvalid = 0xff`, set to 0/1/2 on first publish).

**Verification.** Tests pass. ThreadSanitizer (`-DENABLE_TSAN=ON`) build
should report no races on the dashboard path.

---

### 11. Audit `shared_ptr<event>` ring traffic

**Why this helps performance.** Every `publish_event` pushes the same
`shared_ptr` to multiple worker rings. Each push triggers an atomic
refcount increment; each consumer triggers an atomic decrement. In the
extended preset (5 worker rings) that's up to 10 atomic ops per event.
Atomic ops are ~5–20 ns each on contended cache lines.

**Why behaviour is unchanged (when done right).** Either the event still
gets to every consumer with the same lifetime guarantees (e.g. a
fixed-fanout intrusive count), or you switch to a value-type variant
stored in the ring slot directly (no heap involvement at all).

**Caution.** This is invasive. Profile first to confirm refcount ops show
up in `perf top`. Multiple plausible designs:
- `intrusive_ptr` with a non-atomic counter and external lifetime
  guarantees
- `std::variant<market_event, order_event, fill_event, …>` stored
  inline in each ring slot (eliminates heap)
- `unique_ptr` returned to a pool after the *last* consumer drains
  (requires a fanout barrier)

**Steps**

1. Profile the `shared_ptr` cost. If the atomic-fetch-add/sub pair shows
   in the top 10, proceed. Otherwise skip.
2. Pick a design (variant-in-slot is usually cleanest for fixed event
   types).
3. Refactor `RingBuffer<event_pointer, N>` to `RingBuffer<event_variant, N>`
   or similar.
4. Update every `try_push(ev)` site and every `Worker::on_event` consumer.
5. This is a multi-day refactor — checkpoint with a working build after
   each subsystem (logging worker, risk worker, etc.).

**Verification.** Tests pass. Cross-validate against `engine_backtest` —
golden regression test (`test_golden_regression.cpp`) must produce
identical fills.

---

## Low impact / speculative

These usually buy single-digit-percent gains and require profiler evidence
to justify. Do not attempt without baseline + flame graph in hand.

### 12. Branch hints (`[[likely]]` / `[[unlikely]]`)

**Why this helps performance.** Tells the compiler which branch to favour
in code layout. Saves a mispredict (~15 cycles) on the marked path.
Wins are 1–3 % at best — and often subsumed entirely by PGO (item 3),
which learns the same hints from real data.

**Why behaviour is unchanged.** Pure compiler hint. Generated code makes
the same decision either way; only the code layout changes.

**Steps**

1. After PGO is enabled and validated, identify cold paths that PGO
   couldn't reach (e.g. error paths in seldom-tested code).
2. Mark them explicitly:
   ```cpp
   if (action == risk_action::halt) [[unlikely]] { ... }
   if (questdb_active_)             [[likely]]   { ... }
   ```
3. Be conservative: if PGO already gives the win, the hint is noise.

**Verification.** Microbenchmark the hot path. If it's not measurably
faster, revert.

---

### 13. Cache prefetching (`__builtin_prefetch`)

**Why this helps performance.** When the access pattern is predictable
(iterating a fills vector, walking the orderbook), telling the CPU to
load the next cache line ahead of time hides DRAM latency (~100 cycles).

**Why behaviour is unchanged.** Prefetch is a hint with no architectural
effect. Only the cache state differs.

**Steps**

1. Profile cache misses with `perf stat -e cache-misses,cache-references`.
2. If miss rate is high in identifiable loops (e.g. fill-processing loop
   in `engine::process_order`), insert prefetches:
   ```cpp
   for (size_t i = 0; i < fills.size(); ++i) {
       if (i + 4 < fills.size())
           __builtin_prefetch(&fills[i + 4], 0, 1);
       process(fills[i]);
   }
   ```
3. Measure before/after. If no difference, remove.

**Verification.** Tests pass. Cache-miss rate drops measurably.

---

### 14. CRTP devirtualisation of hot interfaces

**Why this helps performance.** Virtual calls (`IStrategy::on_signal`,
`IDataParser<T>::parse`, `IExecutionAdapter::submit_order`) cost an
indirect branch + an i-cache miss on the vtable lookup. CRTP resolves at
compile time, enabling full inlining.

**Why behaviour is unchanged.** Same dispatch logic, resolved earlier.
Output is bit-identical (modulo inlining-induced FP reordering, which
this codebase doesn't rely on).

**Caution.** Major API surgery. The plug-in registry pattern
(`REGISTER_STRATEGY`, `REGISTER_PROVIDER`) becomes harder to express.
Probably not worth the loss of flexibility unless profiling shows the
indirect call as a top hot spot.

**Steps**

1. Profile `engine_live` with `perf record -e branch-misses` and confirm
   the indirect calls are hot.
2. For `IStrategy`: convert `engine` to a template `engine<Strategy>`,
   propagate up. Provide a pre-instantiated typedef per strategy.
3. Same pattern for `IExecutionAdapter` and `IDataParser`.
4. Keep the registry pattern at the *binary launch* edge only; downstream
   layers see the concrete type.

**Verification.** Golden regression test (`test_golden_regression.cpp`)
produces identical fills. Performance benchmark improves measurably.

---

### 15. SIMD orderbook level scan

**Why this helps performance.** When matching against a deep L2 book,
linear scan over price levels can be vectorised (compare 4–8 levels per
SSE/AVX instruction). Useful only for deep books; meaningless for the
shallow MM-seeded books used in backtest.

**Why behaviour is unchanged.** Mathematical equivalence to the scalar
loop. Same fills at same prices.

**Steps**

1. Profile `OrderBook::match` and confirm it's hot for L2-fed runs.
2. Re-layout price levels as Structure-of-Arrays:
   `std::vector<price_t> level_prices_; std::vector<qty_t> level_qtys_;`
   instead of the current AoS.
3. Implement the price-comparison loop with AVX2 intrinsics
   (`_mm256_cmp_pd` for doubles, `_mm256_cmpgt_epi64` for fixed-point).
4. Provide a scalar fallback for non-x86 builds. Gate on `__AVX2__` define.
5. Cross-validate against the scalar implementation: every backtest must
   produce identical fills.

**Verification.** Golden regression test passes. Microbenchmark shows
3–8× speedup on deep books.

---

### 16. Compile-time strategy/provider specialisation for `engine_live`

**Why this helps performance.** Production deployments of `engine_live`
typically use one strategy and one provider. Knowing them at compile time
lets the compiler inline through every layer that the registry currently
hides behind a virtual call.

**Why behaviour is unchanged.** Same strategy logic, same provider
behaviour. Only the dispatch is resolved at compile time instead of at
process startup via the registry.

**Caution.** Highest complexity-cost in this list. The plug-in registry
becomes a parallel implementation. Almost certainly not worth doing.

**Steps**

1. Add CMake options:
   ```cmake
   set(LIVE_STRATEGY_CLASS "" CACHE STRING "Concrete strategy class for engine_live")
   set(LIVE_PROVIDER_CLASS "" CACHE STRING "Concrete provider class for engine_live")
   ```
2. In `src/bin/engine_live/main.cpp`, when both are set, instantiate the
   concrete types directly instead of going through the registry. Compile
   only when the user opts in.
3. Document the trade-off (no runtime `--strategy` / `--provider` flags
   for that build).

**Verification.** Live binary produces identical orders on a known
workload vs the registry-backed build.

---

## Recommended execution order

If you have a week to spend:

1. **Item 1** (StageTimer baseline) — half a day, mandatory.
2. **Item 2** (mimalloc) — one day.
3. **Item 3** (PGO) — one day for the build setup, plus a training run.
4. **Item 5** (flat_hash_map) — half a day, mostly mechanical.
5. **Item 4** (log_event audit) — half a day to read + verify.
6. **Items 7, 8, 9** (small ring buffers + pre-alloc) — one day total.
7. Re-profile. **Stop here unless the new profile justifies more.**

Do not touch items 6 (symbol interning), 11 (shared_ptr refactor), or any
Low-tier item without profiler evidence pointing at the specific cost.

## Appendix A — PGO workflow

Profile-guided optimisation is gated behind `ENABLE_PGO_GENERATE` and
`ENABLE_PGO_USE` (both default OFF — CI never engages PGO). The two
options are mutually exclusive at configure time. Workflow:

```bash
# Stage 1: build the instrumented binary (counters + LTO).
cmake -B build-pgo \
  -DENABLE_PGO_GENERATE=ON -DENABLE_NATIVE_OPT=ON \
  -DENABLE_MIMALLOC=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-pgo -j --target engine_live

# Stage 2: drive the binary through a representative workload. PGO
# learns from what actually runs in production — pick a workload that
# covers the symbol mix, stream cadence, and order-flow shape you care
# about. .gcda files land in build-pgo/pgo-data automatically.
./build-pgo/engine_live --provider binance --symbol btcusdt \
    --stream kline_1m --strategy sma   # let it run 5–10 minutes

# Stage 3: rebuild with -fprofile-use. Same build dir; the configure
# step re-evaluates the flag set and CMake re-generates Ninja files.
cmake -B build-pgo \
  -DENABLE_PGO_GENERATE=OFF -DENABLE_PGO_USE=ON \
  -DENABLE_NATIVE_OPT=ON -DENABLE_MIMALLOC=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-pgo -j --target engine_live
```

Notes:
- Profile data is keyed by source path → re-run Stage 2 from the same
  build dir before Stage 3, otherwise GCC drops profile coverage and
  warns. `-fprofile-correction` (already wired in) tolerates partial
  drift across LTO partitions but does not paper over a missing
  workload.
- mimalloc and PGO compose cleanly. Mimalloc is third-party, never
  receives `-fprofile-generate`, so its code path runs uninstrumented
  during training. The engine_core .o files inherit the flag through
  `tt_apply_common_flags`.
- Stage 2 wall time roughly mirrors a normal run — instrumentation
  overhead is small (~1 % on this engine).

## Append-only changelog

When you complete an item, add a one-line entry below: date, commit SHA,
measured win on the StageTimer baseline.

| Date | Item | Commit | Measured impact |
|------|------|--------|-----------------|
| 2026-04-27 | 2 (mimalloc) | `be6d4a1` | mean ≈ flat; **max −29 %**; RSS −7.7 % |
| 2026-04-27 | 3 (PGO)      | `ad13ebf` | wall **−1.0 %**; mm_replenish mean −0.8 %; ring_publish mean **−47 %**; tail-stage maxes −70 to −97 % |
| 2026-04-27 | 4 (log audit) | `—` (no code change) | n/a — pre-existing structure already correct |
| 2026-04-27 | 5 (flat_hash_map) | `d1b22d7` | ring_publish mean **−12 %**; pending_drain mean **−21 %**; mm_replenish/market_create max **−14 %** vs Step 2; tests 653/653 pass |
| 2026-04-27 | **Composed 2+3+5** (new ref baseline) | `2e3288b` | wall **−3.0 %**; mm_replenish mean **−2.7 %**; mm_replenish max **−45 %**; ring_publish mean **−58.5 %**; tests 653/653 pass under composed binary |
| 2026-04-27 | E3 (orderbook maps → flat_hash_map) | `66d972c` | neutral within run-to-run noise; orderbook subsystem now consistent with post-Item-5 engine_core convention; 653/653 pass |
| 2026-04-28 | 7 (recent_fills_cache_ deque → ring) | `71bb2a7` | off-hot-path mechanical win; eliminates std::deque chunk allocation in cache_fill / build_dashboard_view recent-fills emit. 665/665 pass excl. EngineStreaming |
| 2026-04-28 | 8 (AdverseSelectionTracker pending_ deque → ring) | `d9203a6` | fixed-cap std::vector ring with in-place compaction in on_mark; zero allocations on the post-fill markout path. AdverseSelection 11/11 incl. cap=3 overflow case |
| 2026-04-28 | 9 (pre-alloc dashboard scratch) | `a2fc746` | swap-buffer dashboard_view_scratch_ keeps vector capacity across refreshes; ctor pre-reserves loose upper bounds; zero allocations on the steady-state event-loop refresh path |
| 2026-04-28 | **Composed 7+8+9 (same-host A/B vs HEAD~3)** | `a2fc746` (HEAD) | ring_publish mean **−10.6%**; pending_drain mean **−6.0%**; strategy mean **−8.0%**; mm_replenish/market_create mean ≈ flat; wall **−1.2%** (43.05 s vs 43.56 s, 3-run mean, host loaded); RSS **+2.2 MiB** (scratch reserves); 665/665 pass excl. EngineStreaming |
