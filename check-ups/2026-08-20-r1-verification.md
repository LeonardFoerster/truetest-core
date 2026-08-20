# R1 verification record — inventory-aware market-making strategy

Date: 2026-08-20
Baseline commit: `23be54e9eaeef5e242b318252667d69afb0aea42`
(`refactor(engine): routerpipeline auftragsattribution entkoppeln`)
Design/spec: [`docs/internal/r1-inventory-aware-market-making.md`](../docs/internal/r1-inventory-aware-market-making.md)

Companion reports:
- [`2026-08-20-r1-benchmark.md`](2026-08-20-r1-benchmark.md)
- [`2026-08-20-r1-queue-sensitivity.md`](2026-08-20-r1-queue-sensitivity.md)

---

## 1. Builds exercised

| Preset | Configuration | Purpose |
|--------|---------------|---------|
| `linux-tests` | Debug, `BUILD_TESTS=ON`, GCC 16.2.1 | unit / property / integration / golden / queue suites |
| `linux-release-low-memory` + `-DENABLE_BENCHMARKS=ON` | Release, LTO off, portable | latency + throughput benchmarks |
| `linux-asan` + `-DENABLE_BINANCE=OFF` | Debug, ASAN **and** UBSAN | sanitizer run over the R1 suites |

## 2. Test results

Full suite, `linux-tests`:

```
ALL 1359 TESTS PASSED (12 skipped)   —   13.4 s
```

The 12 skips are venue suites gated on `HAS_BITGET` / `HAS_BITUNIX` build
flags, unchanged by R1.

R1-specific suites (76 tests):

| Suite | Tests | Covers |
|-------|-------|--------|
| `MMStrategyUnit` | 38 | T01–T25 plus instrument, market-validity and churn-guard edges |
| `MMStrategyProperty` | 5 | P01–P10 over 400 generated cases per property |
| `MMStrategyScenario` | 16 | S01–S15 |
| `MMStrategyIntegration` | 12 | I01–I08, look-ahead, zero-alloc, telemetry |
| `MMStrategyGolden` | 3 | 13 differential fixtures, coverage guard, reproducibility anchors |
| `MMStrategyQueueSensitivity` | 2 | Front/Uniform/Back, markout sign convention |

### Pre-existing full-suite hang (not R1)

`EngineStreaming.PrequeuedFundingReachesEveryThreadedAnalyticsAndDurableLog`
hangs when the full suite runs in registration order (one worker thread spins;
the main thread parks in `futex_wait`). It passes in 488 ms when the
`EngineStreaming.*` suite runs in isolation.

**This is pre-existing.** It was reproduced on the unmodified baseline
(`git stash` of every R1 change, rebuild, rerun) at exactly the same test, and
R1's tests register *after* `EngineStreaming`, so they had not executed when
the stall occurred. All full-suite numbers above therefore exclude that one
test via `--gtest_filter=-EngineStreaming.PrequeuedFundingReachesEveryThreadedAnalyticsAndDurableLog`.

It is out of R1's scope but should be tracked separately — a threaded
funding/analytics test that only hangs under suite ordering is a real defect.

## 3. Sanitizers

`linux-asan` (ASAN + UBSAN in the same binary), R1 suites:

```
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 \
  ./out/build/linux-asan/truetest_tests --gtest_filter='MMStrategy*'

ALL 76 TESTS PASSED   —   984 ms, exit 0
0 AddressSanitizer / LeakSanitizer / UBSAN diagnostics
```

The queue-sensitivity numbers produced by the sanitizer build are byte
identical to those from the plain `linux-tests` build (net P&L 117.5121 /
116.3533 / 115.3305), which is an additional determinism data point across
build configurations.

TSAN was **not** run for R1, deliberately: `evaluate()` creates no thread,
takes no lock, and touches no shared mutable state — the telemetry sink is
injected and called synchronously on the caller's thread. A TSAN run over
single-threaded tests would produce a green result that proves nothing. TSAN
becomes meaningful when the strategy is wired into the engine's threaded
event loop (gap 1 in the design doc), and the command for that job is
`cmake --preset linux-tsan && cmake --build --preset linux-tsan`.

## 4. Gate scripts

| Script | Result |
|--------|--------|
| `scripts/check-hotpath-json.sh` | OK — nlohmann/json confined to the allow-list (R1 uses it only under `tests/`) |
| `scripts/check-layer-deps.sh` | OK — including the new check C (market_maker ↔ strategy/market_making separation) |
| `scripts/check-live-safety-freeze.sh` | OK — no frozen file modified by R1 |
| `scripts/check-mm-reference.sh` | OK — golden expectations match the reference model |

## 5. Performance

Full detail in the benchmark report. Summary, Release build on a Ryzen 9 5900X:

| Ladder | p50 | p95 | p99 | p99.9 | budget |
|--------|-----|-----|-----|-------|--------|
| 1 level | 80 ns | 91 ns | 120 ns | 151 ns | p99 ≤ 50 µs, p99.9 ≤ 100 µs ✅ |
| 8 levels | 141 ns | 161 ns | 241 ns | 291 ns | ✅ |

Allocations per `evaluate()` after warmup: **0** (6,000-call measured window
across ACTIVE / PAUSED / sequence-gap paths at maximum ladder depth, telemetry
sink attached).

## 6. Reproducibility

| Anchor | Value |
|--------|-------|
| Reference `config_hash` | `0x834DD815BAEB86F4` |
| Folded golden `result_hash` (13 cases) | `0x73FA545867D95FFE` |
| 5 consecutive runs | identical `result_hash` |
| `I08_FiveIdenticalReplaysProduceOneResultHash` | 5 × 500-step replay, one hash; a perturbed config yields a different one |

Fixture digests:

| File | SHA-256 |
|------|---------|
| `tests/golden/mm/cases.json` | `35c47a70689ab5c24da56a5c45d8a099700b1b1740ac7bbe1a7c26b7c91fa10d` |
| `tests/golden/mm/reference_config.json` | `01e07644ed2a56fdccf1b071dcb8ac4b480a23880c19e1ab27b901814f002a2a` |
| `tests/golden/mm/expected.json` | `6793ed1c091191b46c26fa6aae88a3a1b3a4b3f944bd99e7f0e16751d71b7610` |
| `tests/reference/mm_strategy_reference.py` | `5cbe4e9f3c8df00b48f4bcd62350bab44025ed2a7fae401e7c36d36424fc0d0c` |

## 7. Acceptance criteria

| ID | Criterion | Status | Evidence |
|----|-----------|--------|----------|
| A01 | Separate `IMarketMakingStrategy` exists | ✅ | `src/strategy/market_making/mm_strategy.h` |
| A02 | Synthetic liquidity seeder and strategy conceptually separated | ✅ | separate modules; `check-layer-deps.sh` check C denies the edge both ways |
| A03 | Fair value implemented | ✅ | mid + microprice, imbalance/flow hooks at weight 0; T12–T14, golden |
| A04 | Inventory reservation-price skew implemented | ✅ | T01–T04, P04/P05, golden |
| A05 | Size skew implemented | ✅ | T02/T03/T06, P04/P05 |
| A06 | Spread controller handles fees/vol/toxicity/latency | ✅ | T19–T23b, S02/S13/S14 |
| A07 | Soft/hard inventory behaviour implemented | ✅ | T05–T08, S10–S12, I04/I05 |
| A08 | Stale / gap / unknown inventory fail closed | ✅ | T09–T11, P09, I06/I07, S08/S09 |
| A09 | Tick / lot / post-only invariants hold | ✅ | T15–T18b, P01/P02/P10 |
| A10 | No heap allocation in `evaluate()` after warmup | ✅ | `EvaluateAllocatesNothingAfterWarmup` — 0 allocations |
| A11 | All unit / property / integration tests green | ✅ | 76 R1 tests, 1359 total |
| A12 | Golden replay deterministic | ✅ | golden result hash pinned; I08 |
| A13 | 5 identical runs → same result hash | ✅ | §6 |
| A14 | 0 look-ahead violations | ✅ | future `event_time` and `receive_time` both rejected; `LookaheadFutureDataCannotAlterAPastDecision` |
| A15 | 0 hard-inventory invariant violations | ✅ | P03 over 400 generated cases; 0 breaches in all three queue runs |
| A16 | 0 ring drops in acceptance runs | ✅ (n/a by construction) | R1 pushes to no ring; the telemetry sink is injected and none of the acceptance runs attaches a ring-backed sink |
| A17 | Queue sensitivity report exists | ✅ | `2026-08-20-r1-queue-sensitivity.md` |
| A18 | Markout report exists | ✅ | same report, 10 ms / 100 ms / 1 s / 5 s, sign convention tested |
| A19 | Benchmark report exists | ✅ | `2026-08-20-r1-benchmark.md` |
| A20 | No live trading enabled | ✅ | no frozen file touched; `target_allows_live_orders()` untouched; R1 has no provider, network, or credential dependency |

## 8. Caveats carried forward

1. The strategy is **not wired into the engine event loop**; the market-state
   and inventory publishers are the follow-up (design doc, gap 1).
2. The hard-limit guarantee depends on the **R3 authoritative order ledger**
   supplying the worst-case fields. Until then
   `safety.require_authoritative_inventory = true` makes the strategy pause
   rather than guess.
3. `engine_live` is still part of the standard build — a pre-existing
   packaging risk, deliberately not refactored here.
4. The queue-sensitivity numbers are **not alpha evidence**; see the caveats
   section of that report.
