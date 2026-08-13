# Memory Safety Check – 2026-08-12

**Modus:** PR / Ad-hoc Feature (backtest reliability defect-closure WIP)  
**Analysierter Pfad:** uncommitted working tree — engine/execution/data/MC/strategy + `tests/test_backtest_defect_closure.cpp`  
**Projekt-Root:** `/home/leonard/work/projects/truetest/core`  
**Report-Ausgabe:** `/home/leonard/work/projects/truetest/core/check-ups/2026-08-12-memory-check.md`  
**Skill-Regel:** read-only analysis; this file is the only intentional write

---

## Zusammenfassung

| Severity | Count | Notes |
|----------|------:|-------|
| **HIGH** | **3** | Design/lifetime: Local modify erase-before-commit + incomplete fail-closed; non-stable `order_pointer` identity after book modify; MC strategy reuse incomplete when `--mc-reuse-objects` (engine-per-trial is OK) |
| **MEDIUM** | **6** | Custom pools unpoisoned (ASAN blind spot); no TSan this run; Hybrid volume budget shared; QueueAware buffer growth; callback TOCTOU residual; filtered ASAN suite only |
| **LOW / INFO** | **~6** | BarView `string_view` contract; mark-map rehash beyond hint; hotpath ASAN threshold inflation; clang-tidy narrow surface; cppcheck absent; stale comments |

**Sanitizer verdict (exercised scope):** **No AddressSanitizer / LeakSanitizer / UBSan runtime errors** on:

- BacktestDefects (47)
- QueueAwareBookAdapter (21)
- HotpathAllocs (2) + HotpathAllocMatrix (6) + HotpathPoolPrewarm (4)
- Engine/Portfolio/MonteCarlo/LocalBook/RealisticFill filter (117)

**Gesamteinschätzung:** Defect-closure WIP did **not** introduce detectable UAF/leaks under ASAN on covered paths. Structural paper-modify / order-identity / MC-reuse debt remains **actionable** for a remediation agent. ASAN green on pool-heavy paths is **not** full proof of freelist safety (no poison annotations). TSan not run (PR mode).

---

## Tool-Ergebnisse

### clang-tidy

| Status | Detail |
|--------|--------|
| **Partial** | `CMAKE_EXPORT_COMPILE_COMMANDS=ON` for `out/build/linux-asan` → `compile_commands.json` present |
| **Ran on** | `src/execution/hybrid_paper_adapter.h`, `queue_aware_book_adapter.h`, `src/orderbook/orderbook.cpp` |
| **Checks** | `clang-analyzer-*`, `bugprone-use-after-move`, `bugprone-dangling-handle`, `bugprone-undefined-memory-manipulation` |
| **Result** | No user-code diagnostics shown; 6 suppressed non-user warnings |

**Gap:** Did not tidy `execution_adapter.h` (modify/sweep), `engine.cpp`, `monte_carlo_controller.cpp`, `market_series.cpp` in this pass.

### cppcheck

| Status | Detail |
|--------|--------|
| **Not installed** | `/usr/bin/cppcheck` absent on host |

### AddressSanitizer + LeakSanitizer (+ UBSan via linux-asan)

**Build:**

```bash
cmake --preset linux-asan   # ENABLE_ASAN=ON ENABLE_UBSAN=ON
cmake --build --preset linux-asan -j --target truetest_tests
# Binary: out/build/linux-asan/truetest_tests (rebuilt 2026-08-12)
```

**Runs (all exit 0; no `ERROR: AddressSanitizer` / `LeakSanitizer` / `runtime error` in logs):**

| Filter | Result |
|--------|--------|
| `BacktestDefects.*` | 47/47 PASS |
| `QueueAwareBookAdapter.*` | 21/21 PASS |
| `*HotpathAllocs*` | 2/2 PASS |
| `*HotpathAllocMatrix*:*HotpathPoolPrewarm*` | 12/12 PASS (incl. matrix + prewarm) |
| `*Engine*:*Portfolio*:*MonteCarlo*:*LocalBook*:*RealisticFill*` | 117/117 PASS |

**Hotpath under ASAN:** Prewarm tests use elevated ASAN ceilings (`TT_HOTPATH_ASAN` → 12k allocs / 40MB). Pass means **no pool grow / no crash**, not zero-heap purity. Prefer non-ASAN `linux-tests` for alloc-count regression.

### ThreadSanitizer

| Status | Detail |
|--------|--------|
| **Not run** | PR-mode; residual race class on workers/DRQ/MC parallel remains untested this run |

### Valgrind

| Status | Detail |
|--------|--------|
| **Not run** | PR-mode |

---

## Subagent-Befunde

### Subagent 1 – Tooling & Evidence

**Agent:** `019ff5ae-a61e-74a0-976b-9d9b80d845fe` (fresh; not implementer)

- ASAN/UBSan filtered green is **credible for exercised filters**, not product-wide safety proof.
- **HIGH residual class:** custom `ObjectPool` / orderbook freelist **without ASan poison** → UAF false negatives.
- **MEDIUM:** filtered suite only (~200 tests under ASAN, not full 1200+); no TSan; stack-resident `engine` in some tests (MC correctly uses `unique_ptr`).
- Hybrid dual-adapter ownership sound; volume double-pass is logic residual.
- Stale `order_pointer` after modify is **memory-safe shared_ptr** but can be **semantically stale** if rebind skipped.
- Hotpath ASAN thresholds: **measurement FP band**, not purity gate.

### Subagent 2 – Ownership & Architectural Lifetime

**Agent:** `019ff5ae-a61e-74a0-976b-9da911cd4498` (fresh; architecture lens)

- **H1 HIGH:** `LocalBookAdapter::modify_order` erases `resting_` before book success; unknown-id path does not hard-return false before `ob_->modify_order` → orphaned book / lost tracking risk.
- **H2 HIGH:** Book modify is cancel+recreate; external `order_pointer` is identity-unstable (valid but stale body).
- **H3 HIGH\***: MC strategy reuse incomplete when enabled (`IStrategy::reset` no-op default); **engine-per-trial is OK**.
- **MEDIUM:** Hybrid dual maps, QueueAware growth, callback TOCTOU residual, mark-map rehash beyond hint.
- **LOW:** `BarView` string_view contract (engine batch path copies symbol immediately — safe today).
- WIP **fixed:** DAY cancel path, hybrid cancel fail-closed, hybrid modify fail-closed for queue-only, no-L2 conservative join, Local rebind on success path.

---

## Detaillierte Findings

### HIGH-01 — LocalBook modify: erase-before-commit + incomplete fail-closed

| Field | Value |
|-------|-------|
| **File** | `src/execution/execution_adapter.h` (~360–394) |
| **Source** | Subagent 2 (Ownership) |
| **Description** | On known resting id, `resting_.erase` runs before `ob_->modify_order`. Failure drops tracking. Unknown id may fall through into book modify without early `return false`. |
| **ASAN** | Unlikely to fire (shared_ptr keeps old body alive) |
| **Impact** | Silent missed bar/MM fills after failed/orphan amend |

### HIGH-02 — Non-stable `order_pointer` identity after book modify

| Field | Value |
|-------|-------|
| **File** | `src/orderbook/orderbook.cpp` (`modify_order` cancel+recreate); consumers in `execution_adapter.h` |
| **Source** | Subagent 2 |
| **Description** | Pre-modify `order_pointer` is not dangling but has stale remaining/price vs book. Rebind via `get_order` is mandatory. |
| **Impact** | Future call sites that cache bodies without rebind → wrong fill qty/price |

### HIGH-03 — MC strategy reuse incomplete reset (when reuse enabled)

| Field | Value |
|-------|-------|
| **File** | `src/simulation/monte_carlo_controller.cpp`; `IStrategy::reset` default |
| **Source** | Subagent 2 |
| **Description** | Fresh `unique_ptr<engine>` per trial isolates engine. Reusing strategy without complete `reset` leaks indicator/position state. Several strategies do not override `reset`. |
| **Impact** | Non-independent trials under `--mc-reuse-objects` |

### MEDIUM-01 — Custom pools/freelists unpoisoned for ASAN

| Field | Value |
|-------|-------|
| **File** | `src/types/object_pool.h`, orderbook node freelist |
| **Source** | Subagent 1 |
| **Description** | No `asan_poison_memory_region` around freelist push/pop → classic freelist UAF may not trip ASAN. |
| **Impact** | False confidence from ASAN-green pool-heavy suites |

### MEDIUM-02 — TSan not run

| Field | Value |
|-------|-------|
| **Source** | Subagent 1 + process |
| **Description** | Workers, DeferredReturnQueue MPSC, standard-threading soft path, MC parallel untested under TSan this run. |

### MEDIUM-03 — Hybrid bar volume budget shared without remainder accounting

| Field | Value |
|-------|-------|
| **File** | `src/execution/hybrid_paper_adapter.h` (~125–133) |
| **Description** | Same `vol_left` passed to local then queue; comment admits under/over-fill ambiguity. |
| **Impact** | Fill realism (not UAF) |

### MEDIUM-04 — QueueAware `pending_fills_` / map growth

| Field | Value |
|-------|-------|
| **File** | `src/execution/queue_aware_book_adapter.h` |
| **Description** | Vectors/maps grow with activity; engine drain contract required. Empty-tape path avoids unbounded level insert (tested). |

### MEDIUM-05 — Callback `[this]` + armed-flag TOCTOU residual

| Field | Value |
|-------|-------|
| **File** | `src/engine/engine.cpp` provider/watchdog callbacks |
| **Description** | Armed flag + join ordering reduces window; residual race if body runs after pass-check during teardown. |

### MEDIUM-06 — Filtered ASAN suite only

| Field | Value |
|-------|-------|
| **Description** | ~197 ASAN tests exercised; full suite under ASAN not re-run this pass (non-ASAN full suite was green earlier in session). |

### LOW-01 — BarView `string_view` into SoA

| Field | Value |
|-------|-------|
| **File** | `src/data/market_series.h` |
| **Description** | Views invalidate after `filter_window` / clear / reallocation. Engine batch loop uses immediately + copies into `market_event`. |

### LOW-02 — Hotpath ASAN alloc threshold FP

| Field | Value |
|-------|-------|
| **File** | `tests/test_hotpath_pool_prewarm.cpp` (`TT_HOTPATH_ASAN` ceilings) |
| **Description** | Known measurement inflation under ASAN; not product heap grow. |

### LOW-03 — Stale comments / docs drift

| Field | Value |
|-------|-------|
| **Examples** | QueueAware header still mentions optimistic join-front in places; MC reuse comments vs fresh-engine practice |

### INFO — Fixed in this WIP (positive)

- Hybrid DAY cancel + latency flush
- Hybrid/queue modify fail-closed for queue-only limits
- Local rebind on successful modify (partial fix for HIGH-02)
- QueueAware partial `remaining_qty`
- Conservative no-L2 join (default)
- MC engine `unique_ptr` (no multi-MiB stack engine)
- `last_mark_prices_` clear+reserve at run start
- Paper tape / multi-sym mid / tick exit LA (correctness; ASAN-clean)

---

## Phasenbasierter Remediation-Plan

**WICHTIG:** Für einen späteren Grok Build Agent **mit Schreibrechten**. Dieses memory-check Run ändert **keinen** Source.

### Phase 1: Vorbereitung & Setup

**Ziel:** Reproduzierbare Sanitizer-Umgebung + Baseline.

1. Projekt-Root: `/home/leonard/work/projects/truetest/core`
2. Non-ASAN baseline (purity / full suite):

```bash
cmake --preset linux-tests
cmake --build --preset linux-tests -j
ctest --test-dir out/build/linux-tests --output-on-failure
./scripts/check-hotpath-json.sh
./scripts/check-layer-deps.sh
./scripts/check-live-safety-freeze.sh
```

3. ASAN rebuild + focused defect suite:

```bash
cmake --preset linux-asan -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build --preset linux-asan -j --target truetest_tests
./out/build/linux-asan/truetest_tests --gtest_filter='BacktestDefects.*:QueueAwareBookAdapter.*'
```

4. Optional full ASAN suite (long):

```bash
ctest --test-dir out/build/linux-asan --output-on-failure
```

5. Install cppcheck if desired: `sudo pacman -S cppcheck` (or distro equivalent).

**Abhängigkeiten:** keine  
**Verify:** all commands exit 0

---

### Phase 2: Kritische Sofort-Fixes (HIGH-01 / HIGH-02 tracking)

**Ziel:** Transactional LocalBook modify + true fail-closed; prevent orphaned book orders.

#### Schritt 2.1 — HIGH-01 LocalBook `modify_order`

**Datei:** `src/execution/execution_adapter.h`

**Konkrete Schritte:**

1. Öffne `src/execution/execution_adapter.h`, `LocalBookAdapter::modify_order`.
2. Implementiere transactionale Semantik:

- Capture `resting_info` copy if present (do **not** erase yet).
- If unknown (`!resting_ && !pending_cancels_`): **`return false`** immediately (true fail-closed).
- Call `ob_->modify_order`.
- On failure: leave original `resting_` intact (or re-insert copy if temporarily removed).
- On success: rebind via `ob_->get_order(order_id)`.

**Beispiel-Struktur (search_replace-Richtung):**

```cpp
// Pseudocode target shape:
bool modify_order(uint64_t order_id, double new_price, double new_qty) override
{
    auto rit = resting_.find(order_id);
    const bool in_resting = rit != resting_.end();
    if (!in_resting && pending_cancels_.count(order_id) == 0)
        return false; // fail-closed: unknown

    resting_info saved{};
    if (in_resting) {
        saved = rit->second;
        // Prefer: keep entry until success; only update book_order pointer after success.
    }

    Price book_price = Price::from_double(new_price);
    quantity book_qty = static_cast<quantity>(std::round(new_qty * qty_scale_));
    if (!ob_->modify_order(order_id, book_price, book_qty))
        return false; // resting_ still valid

    if (in_resting) {
        if (auto body = ob_->get_order(order_id); body && body->get_remaining_quantity() > 0)
            resting_[order_id] = resting_info{body, saved.symbol, saved.side};
        else
            resting_.erase(order_id);
    }
    return true;
}
```

3. Extend `tests/test_backtest_defect_closure.cpp`:

- `LocalModify_UnknownIdReturnsFalse`
- `LocalModify_BookRejectRestoresResting` (if book can reject)
- Keep `FR_LocalModify_RebindsRestingForBarSweep`

4. Build + ASAN:

```bash
cmake --build --preset linux-asan -j --target truetest_tests
./out/build/linux-asan/truetest_tests --gtest_filter='BacktestDefects.FR_LocalModify*:LocalBook*'
```

**Abhängigkeiten:** Phase 1  
**Verify:** new tests green under ASAN; no orphan resting after failed modify

#### Schritt 2.2 — HIGH-02 documentation + assert helper (short-term)

1. Document invariant near `orderbook::modify_order` / `get_order`:  
   *External `order_pointer` is observationally dead after modify/cancel; re-fetch via `get_order`.*
2. Optional debug assert after rebind: `resting_[id].book_order.get() == ob_->get_order(id).get()`.
3. Long-term (Phase 3): in-place amend or `{id, generation}` handles.

---

### Phase 3: Ownership & Lifetime Modernisierung

**Ziel:** Reduce dual-backend and view hazards.

#### 3.1 Hybrid paper single order table (MEDIUM-03 / M1)

**Datei:** `src/execution/hybrid_paper_adapter.h` (+ possibly extract `.cpp` if size grows)

1. Introduce `order_id → backend {Local, Queue}` map OR unified resting store with fill policy.
2. Volume budget: single remaining volume after local fills before queue.
3. Add test: hybrid with residual local market + queue limit → total filled ≤ bar_volume.

#### 3.2 QueueAware caps (MEDIUM-04)

**Datei:** `src/execution/queue_aware_book_adapter.h`

1. Cap `pending_fills_.size()` or fail-closed / halt if engine fails to poll.
2. Keep `trade_candidates_` reuse (already good).
3. Update header comments: default no-L2 is **conservative** (not join-front).

#### 3.3 BarView contract (LOW-01)

**Datei:** `src/data/market_series.h`

1. Document: `BarView` invalid after any mutating series operation.
2. Optional: return `const std::string&` for cold API; keep view only for hot immediate use.

#### 3.4 Dense marks (M4)

**Datei:** `src/engine/engine.h` / `engine.cpp`

1. Prefer `symbol_id` dense vector for marks long-term.
2. Short-term: raise `prepare_mark_prices_for_run` hint from multi-symbol counts.

**Freeze note:** `engine.cpp` changes require `LIVE_SAFETY_CCB_APPROVED` + CCB + soak.

---

### Phase 4: Concurrency & Atomics / MC isolation

**Ziel:** Race coverage + hard-gate MC reuse.

#### 4.1 TSan pass (MEDIUM-02)

```bash
# If preset exists; else configure -DENABLE_TSAN=ON
cmake --preset linux-tsan 2>/dev/null || cmake -B out/build/linux-tsan -DBUILD_TESTS=ON -DENABLE_TSAN=ON
cmake --build out/build/linux-tsan -j --target truetest_tests
./out/build/linux-tsan/truetest_tests --gtest_filter='BacktestDefects.*:SoftPostFill*:MonteCarlo*:Threading*'
```

#### 4.2 HIGH-03 MC strategy reuse gate

**Dateien:** `src/simulation/monte_carlo_controller.cpp`, strategy interface

1. If `reuse_objects_between_trials`:
   - Only allow strategies that implement complete `reset` (allow-list or pure virtual + tests).
2. Or refuse reuse for strategies without override.
3. Keep default: fresh engine per trial.
4. Test: reuse with SMA twice → maps empty after reset (already partial); add breakout/adaptive-hybrid refuse.

#### 4.3 Optional ObjectPool ASAN poison (MEDIUM-01)

**Datei:** `src/types/object_pool.h`

1. Under `#if defined(__has_feature) && __has_feature(address_sanitizer)` poison free slots, unpoison on acquire.
2. Same for orderbook freelist if feasible.
3. Re-run ASAN BacktestDefects + HotpathPoolPrewarm.

#### 4.4 Stack `engine` in tests

Prefer `std::make_unique<engine>(...)` in long-lived / large-pool tests to avoid 8 MiB stack pressure under ASAN.

---

### Phase 5: Verification & Re-Check

**Ziel:** Close the loop.

1. Full non-ASAN suite + three gate scripts (Phase 1 commands).
2. Full or focused ASAN suite (Phase 1).
3. Hotpath non-ASAN:

```bash
./out/build/linux-tests/truetest_tests --gtest_filter='*HotpathAllocs*:*HotpathAllocMatrix*:*HotpathPoolPrewarm*'
```

4. Re-run this skill:

```text
/memory-checks /home/leonard/work/projects/truetest/core/check-ups
```

5. Compare new dated report vs this file; target **0 HIGH** crash/UAF findings; design HIGH-02 may remain as tracked debt until identity-stable orders.

6. If `engine.cpp` touched for remediation: safety skill + freeze token + shadow soak.

---

### Phase 6: Dokumentation & Suppression Management

1. Update `docs/architecture/03-realism.md` for no-L2 conservative join + volume-capped sweeps (if still drifted).
2. Fix QueueAware/MC header comments (L2).
3. Do **not** add ASAN suppressions for freelist without poison — prefer poison or leave as known gap.
4. Link this report from any PR description for freeze-surface review.

---

## Fokus-Abdeckung (truetest-core)

| Area | Covered this run? |
|------|-------------------|
| `engine.cpp` / workers | Yes (diff + greps + ASAN Engine filter; freeze-adjacent) |
| Rings / SPSC | Indirect via hotpath + engine tests; **no TSan** |
| `object_pool` | Hotpath prewarm + design review; **no poison** |
| Execution / hybrid / queue / portfolio | Yes (ASAN + dual subagents) |
| QuestDB clients | Not primary (unchanged WIP focus) |
| Risk / binance kill-DMS | Untouched freeze files; not deep-dived |
| Callback captures | Reviewed (armed flag pattern) |
| string_view / BarView | Reviewed |

---

## Gesamteinschätzung (actionable)

| Claim | Confidence |
|-------|------------|
| Filtered ASAN paths free of instrumentable heap UAF/UB | **High** |
| WIP introduced no new crash-class under covered tests | **High** |
| Paper modify tracking fully correct | **Medium** (HIGH-01 open) |
| Custom freelist UAF free | **Low–Medium** (ASAN blind) |
| Thread-safe under all presets | **Untested** (no TSan) |

**Recommended next agent work:** Phase 2.1 (transactional Local modify) first, then Phase 4.1 TSan, then Phase 3 Hybrid volume accounting.

---

*Generated by `/memory-checks` skill — read-only investigation; only this report written.*  
*Subagents: tooling `019ff5ae-a61e-74a0-976b-9d9b80d845fe`, ownership `019ff5ae-a61e-74a0-976b-9da911cd4498`.*
