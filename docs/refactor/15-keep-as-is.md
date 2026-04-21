# 15 — Components to keep as-is

This file is the **do-not-touch list**. Every component below was reviewed
and deliberately left out of the refactor plan. The reasons are specific
and worth remembering, because some of these look tempting to rewrite.

If a future task description says "also clean up X" and X appears on this
list, push back on the description before doing the work.

---

## Orderbook internals — `orderbook/orderbook.{h,cpp}`

**Why keep it.** This is a textbook-correct implementation for the use case:

- Flat, sorted `std::vector<price_level>` per side. Best bid / ask are
  O(1) at the front. Inserts are O(log N) via binary search, O(N) worst
  case on shift — but N (number of levels) is small and bounded.
- Intrusive doubly-linked list per level (`order_node`) preserves
  time priority without moving data.
- Node pool (4096-slot slab allocator, `NODE_BLOCK_SIZE`) eliminates
  heap churn in the hot path.
- `std::unordered_map<order_id, order_node*>` gives O(1) cancel / modify.

The only thing arguably missing is a cache-locality optimisation via
arena-allocated price levels. That optimisation is only worth it once
profiling says so. Don't speculate.

---

## Ring buffer — `threading/ring_buffer.h`

**Why keep it.** SPSC, power-of-2 capacity, cache-line aligned read and
write positions, built-in high-watermark and drop counters, three
backpressure policies (spin / drop-oldest / assert), optional watermark
callback. It is exactly the right structure for the worker pipeline.

Do not replace with `boost::lockfree::spsc_queue` or `folly::ProducerConsumerQueue` —
they do not add enough to justify a dependency, and they do not give you
the metrics this one exposes.

---

## Analytics — `analytics/analytics.{h,cpp}`

**Why keep it.** Welford's online algorithm for running mean / variance is
the right choice for numerical stability. Rolling Sharpe, rolling drawdown,
per-symbol / per-strategy attribution, benchmark (buy-and-hold),
alpha / beta / information ratio / tracking error are all implemented
correctly.

The `AnalyticsReport` struct is large (lots of fields), but every field
is consumed somewhere (CLI report, JSON export, WS dashboard, golden
tests). Pruning it would break users.

---

## Risk manager — `risk/risk_manager.{h,cpp}`

**Why keep it.** Pre-order and post-fill checks are the correct separation.
Time-windowed limits (daily loss, per-hour trades, per-minute orders)
with proper pruning via `std::deque` are fine. `risk_action` enum with
`pass / reject / halt / unwind` covers the intervention vocabulary.

The one notable gap (no resume channel after halt) is scope-deferred —
mentioned in CLAUDE.md under "Not yet implemented." That is fine for now.

---

## Event logger and replay — `core/event_log.h`

**Why keep it.** Binary format `[type:u8][size:u32][payload]` is compact.
zstd compression is already integrated. The replay path works end-to-end.

The missing piece (time-indexed seek, item C4 in `todo.md`) can be added
without restructuring the format. Do not rewrite the serialiser.

---

## Deterministic seeding — engine construction

**Why keep it.** `engine_config.seed` is threaded through every RNG in the
system (MarketMaker, LocalBookAdapter fill model, any future strategy
RNG). This is what makes the golden regression tests possible.

Do not add any hidden global RNGs. If a new component needs randomness,
take `seed` through its constructor.

---

## Golden regression tests — `tests/test_golden_regression.cpp` + `tests/golden/`

**Why keep them.** A rare asset. Any change that alters backtest results
will trip these tests immediately. Keep them green at all costs.

When a refactor legitimately changes numerics (e.g.
[13-price-qty-unification.md](13-price-qty-unification.md)), regenerate
the golden files and *document the expected drift in the commit message*.

---

## Sanitizer matrix — `ENABLE_TSAN / ASAN / UBSAN`

**Why keep it.** Mutually exclusive with each other (checked at configure
time). TSAN specifically catches data races in the worker pipeline —
invaluable.

Keep the TSAN build green as a release gate. Do not disable any of them
to "fix" a transient test failure.

---

## Worker base class — `threading/worker.h`

**Why keep it.** The spin / pause / yield adaptive backoff is correct for
the current event rates. The consecutive-error tolerance mechanism is
also the right shape — count errors, halt after N, expose the exception
through `get_exception()`.

The only thing worth keeping an eye on is the default `spin_policy::adaptive`
which still spins for ~64 iterations before the pause. For multi-tenant
SaaS deployments, consider defaulting to `spin_policy::yield` so a
badly-tuned tenant does not burn cores.

---

## CMake feature-flag discipline

**Why keep it.** Every optional dependency hides behind `ENABLE_*` and the
core builds with zero external deps. This is a hard rule in CLAUDE.md
and it is worth every bit of the discomfort it causes when adding new
features. It is what makes the "modular" claim in the project description
actually true.

When a new provider task ships, it must add its own `ENABLE_*` flag.
Never fold provider dependencies into the core build.

---

## `utils/retry.h`

**Why keep it.** Exponential-backoff retry with configurable delays is
a tiny, correct utility. Every external connection in the tree uses it.
Do not reinvent.

---

## C API — `api/truetest_api.h`

**Why keep it.** Opaque handle + JSON string input/output is the right
ABI shape for FFI. The Python wrapper (`python/truetest.py`) already
consumes it.

When new configuration fields land, extend the JSON schema consumed by
`tt_create_engine`. Do not add new exported symbols unless absolutely
necessary — each symbol is an ABI surface to maintain.

---

## Checkpointing — `core/checkpoint.h`

**Why keep it in live mode.** Crash recovery from portfolio state is a
real production need. The format works.

In backtest mode, checkpointing is noise — scope it down to live/shadow
only. That is a config-layer change, not a code deletion.

---

## Executive summary

The pattern: **the hot path is fine; the edges need work.** Every data
structure and primitive touched 1,000+ times per second is well-chosen.
The things that need attention are the provider plumbing, the process
surface (CLI / REST), the instrument model, and realism of paper mode —
none of which run inside the event loop.

When in doubt, profile first. The existing benchmarks in
`benchmarks/bench_main.cpp` are the baseline; do not regress them.
