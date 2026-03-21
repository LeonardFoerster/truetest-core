# Multithreading Infrastructure

## Overview

The engine pins distinct responsibilities to dedicated CPU cores. Each core runs a
single-purpose thread. Threads do not share mutable state — they communicate
exclusively through lock-free SPSC (single-producer, single-consumer) ring buffers.

```
                           Core 0
                    ┌─────────────────┐
  market data ───>  │   EVENT LOOP    │  (hot path)
                    │  strategy       │
                    │  orderbook      │
                    │  portfolio      │
                    └───────┬─────────┘
                            │
              ┌─────────────┼─────────────┬─────────────┐
              │ SPSC        │ SPSC        │ SPSC        │ SPSC
              ▼             ▼             ▼             ▼
         Core 1        Core 2        Core 3        Core 4
     ┌───────────┐ ┌───────────┐ ┌───────────┐ ┌───────────┐
     │  LOGGING  │ │   RISK    │ │  MARKET   │ │   STATS   │
     │           │ │  CHECK    │ │  MAKER    │ │ ANALYSIS  │
     └───────────┘ └───────────┘ └───────────┘ └───────────┘
```

## Core 0 — Event Loop (hot path)

This is the latency-critical path. Everything on this core must be allocation-free
and branch-minimal in the steady state.

**Current flow (single-threaded today):**
```
market_event → IStrategy::on_market()
                    │
                    ▼
              order_event → orderbook::add_order() → match_orders()
                                                          │
                                                          ▼
                                                    fill_event → portfolio::on_fill()
```

**Threading rule:** The event loop never blocks, never allocates, never syscalls.
Everything that isn't on the direct path from market data to fill gets pushed to
another core via a ring buffer.

After each event is processed, the loop publishes a copy of the event into the
outbound ring buffers. The receiving threads consume at their own pace. If a
consumer falls behind, it skips — the hot path never waits.

```cpp
// pseudocode — core 0 after processing an event
logging_ring.try_push(event);      // fire-and-forget
risk_ring.try_push(event);         // fire-and-forget
stats_ring.try_push(event);        // fire-and-forget
```

## Core 1 — Logging

Consumes events from `logging_ring`. Formats and writes to disk (or database if
`ENABLE_POSTGRESQL` is on). Batches writes to amortize I/O. This thread is allowed
to block on disk — that's the whole point of isolating it.

**Key constraint:** Logging must never cause back-pressure on Core 0. The ring
buffer is sized large enough (e.g. 64k entries) that bursts are absorbed. If the
ring is full, the event is dropped — a lost log line is preferable to added latency
on the hot path.

## Core 2 — Risk Checking

Consumes order and fill events from `risk_ring`. Maintains a shadow copy of
positions and exposure. If a risk limit is breached, it pushes a `halt` command
back to Core 0 via a separate SPSC ring.

```
Core 0  ──order/fill──>  Core 2 (risk)
Core 2  ──halt/resume──> Core 0
```

The event loop checks the inbound halt ring once per iteration (a single
`try_pop()` — effectively free when empty). If a halt is active, the loop stops
forwarding order events to the orderbook until a resume arrives.

**Why a separate core:** Risk calculations (position limits, drawdown tracking,
correlation checks) can be arbitrarily complex without affecting order-to-fill
latency.

## Core 3 — Market Maker

Consumes market events from a dedicated ring buffer. Runs its own pricing logic
(spread calculation, inventory skew, quoting). Pushes new/cancel orders back to
Core 0's inbound order ring.

```
Core 0  ──market_event──>  Core 3 (market maker)
Core 3  ──order_event──>   Core 0
```

The market maker operates asynchronously — it reacts to market data at its own
pace and submits orders that enter the event loop like any other order. This means
the market maker's latency doesn't add to strategy latency; they run in parallel.

## Core 4 — Statistical Analysis

Consumes all event types. Computes rolling metrics: Sharpe ratio, drawdown, PnL
curves, fill quality, slippage analysis. Results are written to shared memory or a
stats file — never pushed back to the hot path.

This is the only thread that's purely observational. It can be started, stopped,
or swapped without affecting the engine.

## Ring Buffer Design

All inter-thread communication uses a lock-free SPSC ring buffer. One producer,
one consumer, no mutexes, no CAS loops.

```
struct RingBuffer<T, N> {
    T               data[N];          // power-of-two size
    atomic<size_t>  write_pos;        // only written by producer
    atomic<size_t>  read_pos;         // only written by consumer
    // cache-line padding between write_pos and read_pos
};
```

**Properties:**
- `try_push()` returns false if full — producer never blocks
- `try_pop()` returns false if empty — consumer never blocks
- No allocation, no syscall, no lock
- Cache-line padding between `write_pos` and `read_pos` to avoid false sharing

**Sizing:** Each ring is `N` entries where `N` is a power of two. 65536 is a
reasonable default — at 100 bytes per event that's ~6 MB per ring, well within
L3 cache on most systems.

## Thread Affinity

Each thread is pinned to a specific core using `pthread_setaffinity_np` (Linux)
or the platform equivalent. The goal is:

1. Core 0 (event loop) gets an isolated core — no other userspace thread scheduled
   on it. Use `isolcpus` kernel parameter on the deployment machine.
2. Cores 1–4 can share a NUMA node but should each have their own physical core
   (no hyperthreading siblings sharing the hot path core).

```
isolcpus=0          # reserve core 0 for the event loop
```

## What This Means for Current Code

The current `backtest_core::run()` loop is the seed for Core 0. The refactor to
multithreading doesn't change its structure — it adds outbound ring buffer pushes
after each event is processed.

Components that don't exist yet (logging, risk, market maker analytics, stats)
will each be implemented as a class with a `void run()` method that spins on its
inbound ring buffer. They never touch the event loop's state directly.

```cpp
// future shape of each worker
class RiskWorker {
    RingBuffer<event_pointer, 65536>& inbound_;
    RingBuffer<halt_command, 256>&    outbound_;
public:
    void run();   // blocks forever, consuming inbound_
};
```

The backtest mode can either run these threads live (for realistic timing simulation)
or skip them entirely (for pure strategy evaluation speed). A runtime flag controls
this — no code changes needed, just don't spawn the threads.

---

## Keep in Mind: Hardware Detection and Portable Pinning

The core layout described above assumes 5+ physical cores. The binary ships to
users with 4, 8, 12, 20 cores — the pinning strategy cannot be hardcoded.

### The problem with `std::thread::hardware_concurrency()`

It returns the number of **logical** cores the OS exposes. A 6-core/12-thread CPU
reports 12. A 4-core without hyperthreading reports 4. It can also return 0 if the
count is indeterminate.

It does **not** tell you:
- Which logical cores are hyperthreading siblings on the same physical core
- NUMA topology (which cores share L3 cache, which are on different sockets)
- Whether a core is isolated via `isolcpus` or occupied by another process

Pinning the event loop to core 0 and risk to core 1 sounds correct until those
turn out to be hyperthreading siblings sharing the same execution units and L1
cache. That's worse than no pinning at all.

### What's actually needed

For real topology awareness on Linux, read
`/sys/devices/system/cpu/cpu*/topology/`:
- `core_id` — the physical core
- `thread_siblings_list` — which logical cores share a physical core
- `physical_package_id` — the CPU socket

Alternatively, `hwloc` (a C library) abstracts this across platforms and gives
the full tree: socket → NUMA node → L3 → physical core → logical thread.

### Practical approach

The binary should handle this at **runtime**, not compile-time:

1. `hardware_concurrency()` gives the total logical core count as a ceiling
2. If topology info is available (sysfs / hwloc), build a physical core map and
   pick one logical core per physical core — never pin two workers to siblings
3. If topology is unavailable (containers, VMs, exotic platforms), don't pin at
   all — let the OS scheduler handle it, which is fine for everything except
   extreme latency requirements
4. Allow the user to override with explicit core IDs in a config file for
   bare-metal deployments where they know their hardware

### Scaling strategy by core count

| Available physical cores | Pinning strategy |
|--------------------------|------------------|
| 2 | Event loop gets one core, all workers share the other |
| 4–5 | Event loop isolated, risk gets its own, logging + stats share one, market maker gets one if enabled |
| 6–8 | Full 1:1 pinning as described in this document |
| 8+ | 1:1 plus room for per-symbol parallelism, network I/O isolation, or dedicated kernel bypass threads |

The scaling table is a guideline — the actual mapping comes from a config file
with an `auto` default that applies the table based on detected hardware. Users
who care about the last microsecond will override it. Users who don't will never
touch it.
