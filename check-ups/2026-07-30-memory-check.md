# Memory Safety Check – 2026-07-30

**Modus:** PR / Feature (feature/backtest-accuracy merge-ready)
**Analysierter Pfad:** Fill-realism integration after master merge
**Projekt-Root:** `/home/leonard/work/projects/truetest/core`

## Zusammenfassung

- Kritische Findings (HIGH): 1 (engine stack footprint via DeferredReturnQueue)
- Mittlere Findings (MEDIUM): 3 (ASAN preset without Binance; Hybrid raw pointer; dual-book Hybrid vs registry)
- Niedrige / Hinweise: 4
- Gesamteinschätzung: ASan/UBSan clean on accuracy-critical filters after `ENABLE_BINANCE=ON`. Primary crash class was **stack overflow** from ~11 MiB of inline deferred-return queues inside `engine` (default stack 8 MiB). Remediation implemented in-session: heap-allocate queue slots.

## Tool-Ergebnisse

### clang-tidy / cppcheck
Not fully re-run in this pass (time boxed to sanitizer + architecture review). Prior check-ups under `check-ups/` remain valid for broader weekly surface.

### AddressSanitizer + LeakSanitizer + UBSan

| Step | Result |
|------|--------|
| `linux-asan` default preset (no Binance) | HybridExecutor tests fail to compile |
| Reconfigure with `-DENABLE_BINANCE=ON` | Build OK |
| Filter: HotpathAllocs, StopFillPricing, HybridExecutor.RestingLimit, EngineBrackets, Portfolio.Short*, ExitManager.PartialOpener* | **15/15 PASS**, no ASan/UBSan SUMMARY |
| `sizeof(DeferredReturnQueue<>)` before fix | **1 048 704** (~1 MiB) |
| 11 queues in engine | **~11.5 MiB** vs default stack **8 MiB** |

### ThreadSanitizer
Not run (PR mode). Recommended in weekly if touching workers.

## Subagent-Befunde

### Subagent 1 – Tooling & Evidence
- Confirmed stack footprint measurement and ASan-clean accuracy filters.
- HIGH: DeferredReturnQueue inline array.
- MEDIUM: ASAN preset missing Binance; Hybrid `local_book_adapter_` raw view.
- LOW: `lookup_strategy_name` ref footgun; mid not restored on early return in `check_pending_stops`.

### Subagent 2 – Ownership & Architectural Lifetime
- Solid: `handle_engine_fill` funnel, pool lifetime tokens, Hybrid `quote_ids_` cancel-only-ours.
- Structural risks: registry book vs Hybrid private book; MC adapter not cleared on trial reset; global OrderIdGenerator identity class; multi-partial exit residual ledger.
- These are mostly design follow-ups, not crash bugs on the current synthetic-backtest path.

## Detaillierte Findings

| ID | Severity | Location | Description | Status |
|----|----------|----------|-------------|--------|
| H1 | HIGH | `deferred_return_queue.h` | Inline 1 MiB slots × 11 → stack overflow | **Fixed** (heap slots) |
| M1 | MEDIUM | `CMakePresets.json` linux-asan | No ENABLE_BINANCE | **Fixed** |
| M2 | MEDIUM | `hybrid_executor.h` | raw `local_book_adapter_` | Documented; leave for later |
| M3 | MEDIUM | Hybrid vs registry books | Dual liquidity domains | Structural; not this PR |
| L1 | LOW | `engine.cpp` check_pending_stops | mid not restored on halt return | **Fixed** |
| L2 | LOW | order ID collisions in tests | Hybrid test hardcoded id 7 | Fixed earlier (100007) |

## Phasenbasierter Remediation-Plan

### Phase 1: Vorbereitung & Setup
1. Branch `feature/backtest-accuracy`, master is ancestor (`git merge-base --is-ancestor master HEAD`).
2. `ulimit -s unlimited` still recommended until all consumers rebuilt with heap DRQ.
3. Build: `cmake --preset linux-tests && cmake --build --preset linux-tests -j`

### Phase 2: Kritische Sofort-Fixes (implemented)

**Issue HIGH-01 — DeferredReturnQueue stack bloat**

**Konkrete Schritte (done):**
1. Open `src/types/deferred_return_queue.h`
2. Replace `std::array<slot, Capacity> slots_{}` with `std::unique_ptr<slot[]> slots_` allocated in ctor via `std::make_unique<slot[]>(Capacity)`.
3. Rebuild tests; verify `sizeof(engine)` drops by ~11 MiB.
4. Run ASan filter suite without relying solely on huge stacks (still set unlimited for other frames).

**Issue LOW — mid restore on stop halt**

In `check_pending_stops`, restore `last_mid_price_ = bar_mid` before `return` when `process_order` fails.

**Issue MEDIUM — ASAN preset**

Add `"ENABLE_BINANCE": "ON"` to `linux-asan` cacheVariables.

### Phase 3: Ownership & Lifetime Modernisierung (follow-up, not blocking)
1. Unify Hybrid book with OrderbookRegistry **or** document that MM `deliver_mm` is synthetic-only.
2. `IRestingFillSink` capability instead of `dynamic_cast<LocalBookAdapter*>`.
3. Partitioned OrderIdGenerator bands for quotes vs strategy.
4. MC `reset_for_next_trial`: clear `execution_adapters_` or add `adapter->reset()`.

### Phase 4: Concurrency & Atomics
1. Prefer `last_mid_price_.store/load(relaxed)` consistently (already mixed but safe).
2. Optional `MidAnchor` RAII for stop/bracket paths.

### Phase 5: Verification & Re-Check
```bash
ulimit -s unlimited   # belt-and-suspenders
cmake --build out/build/linux-tests -j
./out/build/linux-tests/truetest_tests --gtest_filter='*HotpathAllocs*:*StopFillPricing*:*HybridExecutor*:*ObjectPool*'
./scripts/check-hotpath-json.sh
./scripts/check-layer-deps.sh
./scripts/check-live-safety-freeze.sh
# ASAN:
cmake -B out/build/linux-asan -DENABLE_ASAN=ON -DENABLE_UBSAN=ON -DBUILD_TESTS=ON -DENABLE_BINANCE=ON
cmake --build out/build/linux-asan -j --target truetest_tests
./out/build/linux-asan/truetest_tests --gtest_filter='*HotpathAllocs*:*StopFillPricing*:*HybridExecutor.RestingLimit*'
```

### Phase 6: Dokumentation
- Note stack footprint history in this file.
- Prefer heap `engine` in new long-lived API paths (already true for `truetest_api`).

## Implemented in this session

1. Heap-allocated `DeferredReturnQueue` slots.
2. `check_pending_stops` restores mid on early halt return.
3. `linux-asan` enables Binance.
4. HybridExecutor test order_id collision (prior commit).
