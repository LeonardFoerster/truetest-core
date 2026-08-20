# R1 benchmark report — inventory-aware market-making strategy

Date: 2026-08-20
Subject: `truetest::mm::InventoryAwareMarketMakingStrategy::evaluate()`

## Reference hardware and build

| Item | Value |
|------|-------|
| CPU | AMD Ryzen 9 5900X, 12 cores / 24 threads, observed 4954 MHz |
| Caches | L1d 32 KiB ×12, L1i 32 KiB ×12, L2 512 KiB ×12, L3 32 MiB ×2 |
| RAM | 31 GiB |
| OS / kernel | Linux 6.18.42-1-cachyos-lts, x86_64 |
| Compiler | GCC 16.2.1 20260810 |
| Build preset | `linux-release-low-memory` + `-DENABLE_BENCHMARKS=ON` (Release, LTO off, no `-march=native`) |
| CPU governor | `powersave` — frequency scaling **was enabled**, Google Benchmark warned accordingly |
| Load average at run | 2.63 (1 min) |

The portable Release preset was used rather than `linux-benchmarks`, because
that preset is `CMAKE_BUILD_TYPE=Debug` with `ENABLE_DEBUG=ON` and its numbers
would not be a valid performance claim.

## Regression budget

`p99 <= 50 us`, `p99.9 <= 100 us`. This is a regression budget for this
repository on this hardware, not a claim about any venue's requirements.

## Results

`BM_MMStrategy_Evaluate` — steady-state throughput, no per-call timing
instrumentation. Inputs sweep the full utilisation range including both hard
limits, so every branch is exercised.

| Ladder levels | Mean time / call | Mean CPU / call | Iterations |
|---------------|------------------|-----------------|------------|
| 1 | 61.0 ns | 60.7 ns | 11,351,588 |
| 4 | 90.7 ns | 90.2 ns | 7,898,758 |
| 8 (compile-time max) | 135 ns | 134 ns | 5,261,449 |

`BM_MMStrategy_LatencyDistribution` — 1,000,000 calls per iteration, 3
iterations, per-call `steady_clock` deltas sorted into percentiles. Inputs are
materialised before the measured window, so the window contains nothing but
`evaluate()`; no logging is flushed and no telemetry sink is attached.

| Ladder levels | p50 | p95 | p99 | p99.9 | max | budget p99 | budget p99.9 |
|---------------|-----|-----|-----|-------|-----|------------|--------------|
| 1 | 80 ns | 91 ns | 120 ns | 151 ns | 9.28 us | 50 us ✅ | 100 us ✅ |
| 8 | 141 ns | 161 ns | 241 ns | 291 ns | 35.6 us | 50 us ✅ | 100 us ✅ |

Headroom against the budget is roughly 400× at p99 and 340× at p99.9 for a
one-level ladder, and 200×/340× at the maximum ladder depth.

### Reading the numbers honestly

- Every percentile **includes two `steady_clock::now()` calls** (~20–40 ns on
  this machine), so the reported latency overstates the strategy's own cost.
  The uninstrumented `BM_MMStrategy_Evaluate` means are the cleaner figure.
- The `max` column (9.3 us / 35.6 us) is scheduler preemption and frequency
  scaling, not strategy work: it is a single sample out of a million and does
  not move p99.9.
- CPU frequency scaling was left enabled (`powersave` governor). Re-running
  under `performance` would tighten the distribution; the budget is met by
  such a margin that pinning was not needed to reach a verdict.
- The machine had a background load average of ~2.6 during the run.

## Allocation behaviour

`MMStrategyIntegration.EvaluateAllocatesNothingAfterWarmup` counts global
`operator new` calls (`tests/helpers/alloc_counter.h`) across a 6,000-call
measured window covering ACTIVE, PAUSED (stale) and sequence-gap paths at the
maximum ladder depth, with a telemetry sink attached:

**0 allocations, 0 bytes per `evaluate()` after warmup.**

The decision payload is a fixed-capacity value type
(`fixed_vector<quote_intent, 16>` + `fixed_vector<quote_reason, 10>`), returned
by value; there is no pool to exhaust and no container that can grow.

## Reproduce

```bash
cmake --preset linux-release-low-memory -DENABLE_BENCHMARKS=ON
cmake --build out/build/linux-release-low-memory --target truetest_benchmarks -j 4
./out/build/linux-release-low-memory/truetest_benchmarks \
    --benchmark_filter='MMStrategy' --benchmark_min_warmup_time=0.2

# allocation proof
./out/build/linux-tests/truetest_tests \
    --gtest_filter='MMStrategyIntegration.EvaluateAllocatesNothingAfterWarmup'
```
