# C++ Code Review — Bad / Good Examples (truetest-core)

Lenses cite these by id (e.g. **PERF-01**). Examples are illustrative; match the
*class* of bug, not the exact snippet.

Priority when examples conflict: **correctness > performance > simplicity > readability**.

---

## Performance

### PERF-01 — Heap on hot path

**Bad**

```cpp
void on_trade(const Trade& t) {
  auto* e = new TradeEvent(t);              // heap
  std::vector<Level> levels = book.top(5);  // may grow
  std::string s = std::to_string(t.px);     // alloc
  publish_raw(e);
}
```

**Good**

```cpp
void on_trade(const Trade& t) {
  auto e = acquire_pooled<TradeEvent>(trade_pool_);
  e->reset(t);
  // top_n into pre-sized buffer / fixed array if needed for strategy
  publish_event(std::move(e));
}
```

**Review note:** Flag any `new` / growing `vector` / `string` construction on HOT
paths (engine publish, orderbook match, strategy `on_*`, pre-trade risk).

---

### PERF-02 — JSON / heavy formatting on hot path

**Bad**

```cpp
void on_order_update(const Order& o) {
  nlohmann::json j = o;                    // FORBIDDEN on hot path
  log_info("order {}", j.dump());          // alloc + I/O on event
}
```

**Good**

```cpp
void on_order_update(const Order& o) {
  // Structured POD / ring message to logging worker; format off hot path
  log_ring_.try_push(OrderLogSample{o.id, o.px, o.qty, o.state});
}
```

**Review note:** `scripts/check-hotpath-json.sh` must stay green. Expanding the
JSON allow-list is Ask-first.

---

### PERF-03 — Silent pool grow under load

**Bad**

```cpp
pool.prewarm(1024);
// later under pressure:
pool.grow();  // or default grow-on-exhaust — latency spike / hides capacity bugs
```

**Good**

```cpp
pool.prewarm(max_concurrent + headroom);
pool.set_forbid_runtime_grow(true);  // exhaust → fail closed / halt
```

---

### PERF-04 — Second producer on SPSC ring

**Bad**

```cpp
// Thread A
critical_ring.try_push(a);
// Thread B  — also pushes the same ring
critical_ring.try_push(b);
```

**Good**

```cpp
// Single producer (engine loop) → SPSC → single consumer
// Fan-in: multiple SPSC rings drained by one consumer — not MPMC "for convenience"
```

---

### PERF-05 — Shared_ptr / type erasure on micro-hot loop

**Bad**

```cpp
void on_tick(const Tick& t) {
  auto cb = std::make_shared<std::function<void()>>([&]{ strategy_->on_tick(t); });
  (*cb)();
}
```

**Good**

```cpp
void on_tick(const Tick& t) {
  strategy_->on_tick(t);  // direct; or compile-time strategy template if measured
}
```

---

### PERF-06 — Measured claims without numbers

**Bad (review comment to reject)**

> “This should be faster because we use a hash map.”

**Good (acceptable claim shape)**

> “Bench `Orderbook.Apply` p99 1.2µs → 0.9µs on linux-native; hotpath alloc tests still 0 allocs.”

---

## Correctness

### CORR-01 — Safety path “helpfulness” (retry / auto-clear halt)

**Bad**

```cpp
if (kill_switch_triggered) {
  schedule_retry_with_backoff();   // NEVER
  halt_flag_ = false;              // NEVER auto-clear
}
```

**Good**

```cpp
if (kill_switch_triggered) {
  halt_flag_.store(true, std::memory_order_release);  // terminal; restart only
  // cancel / notify / diagnostics — no resume path
}
```

---

### CORR-02 — Use-after-recycle from pool

**Bad**

```cpp
auto e = acquire_pooled<TradeEvent>(pool);
publish_event(std::move(e));
// consumer may recycle e; producer still holds raw pointer
last_event_ = e.get();  // dangling
```

**Good**

```cpp
auto e = acquire_pooled<TradeEvent>(pool);
// copy out any fields needed before handoff, or use generation/id tokens
const auto id = e->id;
publish_event(std::move(e));
// later: look up by id in authoritative structure, not stale pointer
```

---

### CORR-03 — Non-deterministic MC / shared mutable across trials

**Bad**

```cpp
static std::mt19937 rng{42};  // shared across parallel trials
void run_trial(int trial) {
  // mutates global rng — races + non-reproducible trials
}
```

**Good**

```cpp
void run_trial(uint64_t base_seed, int trial) {
  std::mt19937_64 rng{mix_seed(base_seed, trial)};
  // all randomness from this rng only; no shared mutable trial state
}
```

---

### CORR-04 — Live gate bypass

**Bad**

```cpp
if (config.allow_live_orders) {  // runtime switch resurrects dead code in backtest
  send_live(order);
}
```

**Good**

```cpp
// Compile-time only via TT_TARGET; target_allows_live_orders() is constexpr
if constexpr (target_allows_live_orders()) {
  send_live(order);
}
```

---

### CORR-05 — Dropping events on full ring without fail-closed policy

**Bad**

```cpp
if (!ring.try_push(e)) {
  // silent drop
  return;
}
```

**Good**

```cpp
if (!ring.try_push(e)) {
  // policy explicit: count + halt / backpressure / fail closed per design
  on_ring_overflow_fail_closed();
}
```

**Review note:** Silent loss on market/safety rings is usually **blocker**.

---

### CORR-06 — Wrong risk check order (futures)

**Bad**

```cpp
risk_manager.check(order);
futures_venue_check.check(order);  // venue constraints too late
```

**Good**

```cpp
// Venue FuturesRiskCheck before RiskManager on futures hot path
futures_venue_check.check(order);
risk_manager.check(order);
```

---

## Simplicity

### SIMP-01 — Parallel subsystem instead of existing API

**Bad**

```cpp
// New ad-hoc event bus beside engine publish/pools
my_bus_.emit(std::make_unique<MyEvent>(...));
```

**Good**

```cpp
auto e = acquire_pooled<MyEvent>(pool_);
e->reset(...);
publish_event(std::move(e));
```

---

### SIMP-02 — Abstraction for a single call site

**Bad**

```cpp
struct IFillHandler { virtual void on_fill(const Fill&) = 0; virtual ~IFillHandler() = default; };
struct FillHandlerFactory { static std::unique_ptr<IFillHandler> create(Config); };
// … 4 files, one production implementation, one test mock that could be a lambda
```

**Good**

```cpp
// Direct function or existing strategy/execution hook until a second real impl appears
void on_fill(const Fill& f) { /* … */ }
```

---

### SIMP-03 — Boolean soup

**Bad**

```cpp
if (a && !b || (c && d) && !e || f == 3) {
  place();
}
```

**Good**

```cpp
const bool entry_ok = signal_ready && !in_position;
const bool risk_ok = within_limits && !halted;
if (entry_ok && risk_ok) {
  place();
}
```

---

## Readability

### READ-01 — Narrating comments vs invariants

**Bad**

```cpp
i++; // increment i
// set flag to true
ready = true;
```

**Good**

```cpp
// Invariant: ready is set only after prewarm + forbid_runtime_grow(true).
ready = true;
```

---

### READ-02 — Unclear ownership in signatures

**Bad**

```cpp
void adopt(Order* o);  // who deletes? pool? caller? shared?
```

**Good**

```cpp
void adopt(PooledPtr<Order> o);           // ownership transfer explicit
// or
void observe(const Order& o);             // non-owning
```

---

### READ-03 — Giant function without seams

**Bad:** 200-line `Engine::on_event` mixing parse, risk, match, metrics, JSON snapshot.

**Good:** Keep hot path linear and short; extract cold snapshot/metrics to workers;
name intermediate steps with small functions that do not allocate.

---

## Layering / architecture

### LAYER-01 — Venue leakage into generic core

**Bad**

```cpp
// src/engine/engine.cpp
#if HAS_BINANCE_FUTURES
  binance_specific_reconcile();
#endif
```

**Good**

```cpp
// Provider implements IProvider + safety hooks; engine calls interfaces only
provider_->reconcile_if_needed();
```

---

### LAYER-02 — New source not registered

**Bad:** Add `src/foo/bar.cpp` but forget `cmake/Sources.cmake` (no globs).

**Good:** Register in `cmake/Sources.cmake`; configure + build affected targets.

---

## Test ideas tied to examples

| Id | Falsify with |
|----|----------------|
| PERF-01 | `ctest -R hotpath` / alloc counters; bench filter |
| PERF-03 | Force pool exhaust under `forbid_runtime_grow`; expect fail-closed |
| PERF-04 | ThreadSanitizer or sole-producer audit |
| CORR-01 | Unit test: halt stays set after kill path |
| CORR-03 | Two MC trials same base seed+trial → identical `seed_used` / equity path |
| CORR-05 | Ring full → halt or explicit counter, never silent zero |
| LAYER-01 | `./scripts/check-layer-deps.sh` |

---

## Finding write-up example (copy shape)

```markdown
### F1 — temporary vector on match path
- Severity: high
- Lens: performance
- File: src/execution/queue_aware_book_adapter.h:218
- Path tags: HOT
- Problem: `std::vector<Level>` built per fill (PERF-01)
- Why it matters: heap jitter on match path; breaks zero-alloc invariant
- Evidence: vector constructed inside `on_fill` called from book apply
- Fix direction: fixed `std::array` or pre-sized buffer member; no grow
- Counterexample / test idea: hotpath alloc test around synthetic fills
```
