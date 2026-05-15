# Engine optimization workflow

How to find, measure, and lock in performance wins in the engine
without breaking correctness. Companion to `docs/performance.md`
(architecture-level decisions) and `docs/perf-baseline.md` (numerical
reference points).

This is a **methodology**, not a list of optimizations. The fastest
way to make the engine slower is to optimize the wrong stage. Always
measure first.

## When to read this

- A backtest that used to take N minutes now takes 2N.
- A new feature has shipped and you want to confirm no perf regression.
- Live mode is dropping ring-buffer messages under load.
- You're considering an architectural change (lock-free queue, SIMD,
  thread pinning) and want a defensible before/after.

## When *not* to optimize

- Correctness work isn't done. Realism gaps (`docs/realism-wiring.md`)
  and validation gaps (`docs/strategy-validation.md`) move the floor;
  optimizing on a dishonest fill model just makes you wrong faster.
- The bottleneck is the disk / network / database, not the engine
  hot path. Profile first.

## Build for measurement

```bash
cmake -B build-perf \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_DEBUG=ON \
    -DENABLE_NATIVE_OPT=ON \
    -DENABLE_BENCHMARKS=ON
cmake --build build-perf -j
```

Flag rationale:

| Flag | Why |
|------|-----|
| `Release` | `-O3`, no asserts; mandatory for perf numbers |
| `ENABLE_DEBUG` | enables `stage_timer`, `ring_stats`, `memory_info`, `copy_tracker` instrumentation. **Costs ~3-5% throughput** but the visibility is worth it during investigation. Disable for the final published number. |
| `ENABLE_NATIVE_OPT` | `-march=native` + extra unrolling. Required to match `docs/perf-baseline.md` numbers. |
| `ENABLE_BENCHMARKS` | builds the Google Benchmark suite for microbenchmarks of specific stages. |

Sanitizers (`-DENABLE_ASAN=ON`, `-DENABLE_TSAN=ON`) are mutually
exclusive with optimization runs — use them for correctness only.

## Reference workload

The canonical benchmark in `docs/perf-baseline.md` is:

- **Data**: 50k-bar synthetic CSV.
- **Strategy**: SMA, default parameters.
- **Mode**: backtest, single-threaded (`inline` preset).
- **Recorded baseline**: 36.7s wall, ~1.4k events/sec.

```bash
./build-perf/engine_backtest \
    --provider local --path market_data.csv \
    --strategy sma \
    --threading inline \
    --status-format off
```

Use this as the anchor for "did my change make things faster or
slower in absolute terms." Run 5+ times, take the median, document
the machine the number was taken on.

## Read the instrumentation

### Stage timings (`src/debug/stage_timer.h`)

Nine stages tracked at nanosecond precision:

| Stage | What it covers |
|-------|----------------|
| `market_create` | parse + construct `market_event` |
| `strategy` | strategy `on_bar` / `on_tick` callback |
| `orderbook` | match against the book, generate fills |
| `fill_processing` | portfolio update, fill event emission |
| `ring_publish` | enqueue to worker rings |
| `risk_check` | pre / post-fill risk gates |
| `mm_replenish` | MM book reseeding |
| `stop_check` | exit bracket evaluation |
| `pending_drain` | latency-deferred order release |

Per stage: call count, total ns, avg ns, max ns, jitter (max-vs-avg
ratio). High `total_ns` = candidate for optimization. High jitter =
candidate for cache-locality / branch-prediction work.

Snapshot is also available live in the rich TUI debug tab and is
printed at engine shutdown.

### Ring stats (`src/debug/ring_stats.h`)

Per worker ring:

- `total_pushed`, `total_popped` — sanity check (popped should track
  pushed within the ring's depth).
- `drop_count` — **never zero in production**. If non-zero in
  benchmark mode the producer is overrunning the consumer.
- `high_water_mark` — peak occupancy; > 50% of capacity (65536) is
  warning territory.
- `avg_occupancy` — sustained load.

A worker with high HWM is the bottleneck, not the hot path. Optimizing
the hot path will only make the drops worse.

### Memory info (`src/debug/memory_info.h`)

RSS, peak RSS, heap, computed pool/ring footprints. Useful to confirm
no leak / no unexpected allocation pattern. The hot path should not
allocate; if RSS climbs over a long run, something is leaking.

### Copy tracker (`src/debug/copy_tracker.h`)

Counts allocations and moves on the hot path. **Should be near zero
on a healthy hot path.** Non-zero counts on a high-frequency code
path point at a missing `move`, missing reserve, or accidental copy.

## Investigation playbook

Standard sequence:

1. **Reproduce the regression.** Run the reference workload at known
   git revisions; confirm the slowdown is real and not noise. Run
   each revision 5+ times.
2. **Read the stage timer report.** The stage with the largest
   `total_ns` increase between revisions is the suspect.
3. **Read the ring stats.** If a worker has gone from 0% drops to
   non-zero, the bottleneck moved into a worker — check
   `risk_worker`, `stats_worker`, `logging_worker`,
   `questdb_worker` instrumentation.
4. **Read the copy tracker.** If hot-path copies have appeared,
   that's almost always the cause.
5. **Microbench the suspect stage** with the Google Benchmark suite.
   Land a fix.
6. **Confirm fix on the reference workload.** Median across 5+ runs
   should match the pre-regression baseline.
7. **Lock it in** — see "Locking in wins" below.

## Common optimization wins, by stage

These are areas where past investigation has found leverage. Use as a
hint, not a checklist — always measure your specific case first.

- **`market_create`** — parsing JSON / CSV. Already hand-rolled; if it
  shows up, look for missing `string_view`, redundant string copies,
  or unnecessary `nlohmann/json` calls (CI-banned on the hot path
  via `scripts/check-hotpath-json.sh`).
- **`strategy`** — strategy code itself. Profile the strategy in
  isolation; this is application code, not engine code.
- **`orderbook`** — book matching. Watch for O(n) scans where
  log-time would suffice; verify price-level data structure choice
  matches the workload.
- **`fill_processing`** — portfolio updates. Often dominated by
  symbol-keyed map lookups; consider hash strategies or contiguous
  storage if symbol count is small and known.
- **`ring_publish`** — worker enqueue. If this dominates, either the
  ring is contended (shouldn't be — it's SPSC) or you're publishing
  too many events; consider batching.
- **Worker-side bottleneck** — `RiskWorker`, `StatsWorker` etc.
  shifting load to a worker preset is *the* lever for relieving the
  hot path. See `src/threading/thread_preset.h`.

## Microbenchmarks

Google Benchmark suite, built with `ENABLE_BENCHMARKS=ON`:

```bash
./build-perf/bench_<target> --benchmark_min_time=2s
```

Add a microbench when you're optimizing a specific function in
isolation and want a fast iteration loop. Don't replace the reference
workload with microbenches — they miss cache effects, ring contention,
and inter-worker dynamics.

## Locking in wins

Every meaningful win lands with three things:

1. **A reference-workload measurement** in the PR description: median
   of 5+ runs, before vs after, machine spec, build flags.
2. **An update to `docs/perf-baseline.md`** if the new number is the
   new floor. Note the date and commit.
3. **A regression guard** — at minimum a microbench for the changed
   function. Ideally a CI check that fails if the reference workload
   regresses past a threshold.

Without (3), the win evaporates the next time someone refactors that
file.

## Multi-thread / preset tuning

When measuring with the threaded presets (`light`, `standard`, `full`,
`extended`):

- Always pin to specific cores via `--cpu-affinity` and document
  which cores. Otherwise the OS scheduler will mask perf changes
  with thread-migration noise.
- Use `--spin-policy spin` for benchmark runs (lowest latency); use
  `adaptive` for production-realistic numbers.
- Compare against `inline` (single-threaded) — if the threaded
  preset is slower on the reference workload, the worker overhead is
  exceeding the parallelism gain. That's a real signal worth
  investigating.

## Live-path specific gotchas

When optimizing the live path (`engine_live`):

- The hot path runs alongside Boost.Beast WS plus OpenSSL —
  prefer measuring with `BinanceReplayTransport` against a recorded
  tape rather than a live mainnet connection. Same code path,
  reproducible inputs.
- TLS session resumption (cached per `BinanceRestClient` and
  `BinanceUserDataTransport`) is critical — confirm reconnect
  scenarios still hit the cache.
- Order-encoder caching (`BinanceOrderEncoder`) prepares the
  symbol+side+type prefix once. If a refactor breaks that, the
  per-order REST cost goes up sharply.

## What this workflow does *not* cover

- **Latency tail analysis** under live load (P99.9, microbursts).
  Needs sampling profilers (`perf`, `BPF`) attached to a live
  process — out of scope for this doc; covered in
  `docs/performance.md`.
- **Memory bandwidth / NUMA tuning** — relevant only on multi-socket
  hardware; document case by case if it ever matters.

## See also

- `docs/performance.md` — architectural performance decisions.
- `docs/perf-baseline.md` — numerical reference points; the floor
  every optimization measures against.
- `src/debug/` — instrumentation source.
- `src/threading/thread_preset.h` — worker preset definitions.
- `docs/strategy-validation.md` — validate correctness *first*, then
  optimize.
