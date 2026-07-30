# Memory Safety Check – 2026-07-30

**Modus:** PR / Feature (post-merge of `feature/backtest-accuracy` + residual-risks on `master`)  
**Analysierter Pfad:** `/home/leonard/work/projects/truetest/core` (tip `060abdc`)  
**Projekt-Root:** `/home/leonard/work/projects/truetest/core`  
**Report-Ziel:** `check-ups/` (user path `@check-ups`)  
**Skill:** memory-checks (read-only; only this file written)

## Zusammenfassung

| Severity | Count | Notes |
|----------|------:|-------|
| **HIGH** | 2 | Live funding dual-producer on SPSC rings + DRQ; ControlBlockPool lacks lifetime token |
| **MEDIUM** | 4 | Hybrid quote `make_shared`; `in_use` on intentional leak; fill vectors; halt API footgun |
| **LOW** | 4 | ASan alloc-ceiling flake; DRQ overflow mutex; watermark callback; intentional late-drop LSan |
| **FIXED (prior)** | 3 | DRQ heap slots; ASAN Binance preset; Hybrid raw `LocalBookAdapter*` removed |

**Gesamteinschätzung:** Accuracy-merge memory crash class (engine stack overflow via inline DRQ) is **closed**. Focused ASan suites show **no UAF / OOB / stack-overflow**. Remaining HIGH items are **structural concurrency/lifetime** on the **live funding** path and **control-block** late-drop asymmetry — not introduced as regressions of fill-realism, but open for remediation. Non-ASan hotpath alloc tests pass; under ASan, absolute alloc ceiling for L2Burst is tight due to wall-clock dashboard refresh.

---

## Tool-Ergebnisse

### clang-tidy
- `compile_commands.json` **absent** under `out/build/linux-tests` and `out/build/linux-asan`.
- clang-tidy present (`/usr/bin/clang-tidy`) but **not productively runnable** without compile DB.
- **Action:** export compile commands on next configure (`-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`).

### cppcheck
- **Not installed** on host.

### AddressSanitizer + LeakSanitizer (+ UBSan via linux-asan preset)

Build:

```bash
cmake --build out/build/linux-asan -j"$(nproc)" --target truetest_tests
# Binary rebuilt after residual fixes (engine.cpp / execution_adapter.h newer)
```

Focused filter:

```text
*HotpathAllocs*:*HotpathPoolPrewarm*:*StopFillPricing*:*HybridExecutor*:
*EngineBrackets*:*TickToTradeSafety*:*DeferredReturn*:*ObjectPool*:*Ring*
```

| Result | Detail |
|--------|--------|
| ObjectPool (15) | PASS (including LateDrop*) |
| DeferredReturnQueue (2) | PASS |
| RingBuffer (12) | PASS |
| StopFillPricing (6) | PASS |
| EngineBrackets (3) | PASS |
| TickToTradeSafety (7) | PASS |
| HotpathAllocs (2) | PASS |
| HybridExecutor (7) | PASS |
| HotpathPoolPrewarm | 2/3 PASS; **FAIL** `L2Burst_NoControlBlockHeapAllocs` |
| **UAF / heap-buffer / stack-overflow** | **None** |
| LSan at exit | **17 B** intentional late-drop (see F2) |

**FAIL detail:**

```text
HotpathPoolPrewarm.L2Burst_NoControlBlockHeapAllocs
  Expected: (snap.count) <= (9000u), actual: 9630 vs 9000   # first ASan run
  Expected: (snap.count) <= (9000u), actual: 9598 vs 9000   # clean re-run
```

Sibling under ASan: `L2BurstUsesPooledEvents_NoRuntimeGrow` **PASS** (`grow_count == 0`).

Non-ASan (`out/build/linux-tests`): HotpathAllocs + HotpathPoolPrewarm **5/5 PASS**.

### ThreadSanitizer
Not run (PR mode). **Recommended** after fixing dual-producer funding path (Phase 4).

### Weitere
- Prior report content (same-day earlier draft) retained as history for DRQ fix evidence.
- Residual-risks disposition: `check-ups/2026-07-30-residual-risks-resolution.md`.

---

## Subagent-Befunde

### Subagent 1 – Tooling & Evidence

- **No crash-class ASan findings** on accuracy/hotpath/object-pool/ring filters.
- **F1 LOW:** ASan L2Burst alloc count exceeds 9000 because `publish_event` → wall-clock `dashboard_builder_->refresh_if_due()` (~100 ms) runs many times under ASan slowdown (~5 s for 4000 L2 updates ≈ 50 rebuilds). Non-ASan finishes faster → under ceiling. **Not** control-block heap grow.
- **F2 intentional:** LSan 17 B = `StringWidget` string body for `"late-drop-target"` when ObjectPool Returner skips `~T` after dtor (documented safe leak).
- Prior HIGH DRQ stack overflow **FIXED** (`unique_ptr<slot[]>`).
- Hybrid raw pointer residual **FIXED** (virtual dispatch).
- Static analysis gap (no compile DB / no cppcheck).

**Tooling risk rating for tip:** **LOW** for crash/UAF; open work is test budget + intentional-leak LSAN hygiene.

### Subagent 2 – Ownership & Architectural Lifetime

**Event ownership map (solid core path):**

```
engine acquire_pooled → placement new + shared_ptr(Returner{lifetime,epoch})
  → publish_event → EventRing try_push (shared_ptr copy)
  → worker try_pop → on_event → drop → Returner: ~T + defer_release(DRQ)
  → engine drain_deferred_returns / next acquire → free list
```

**Solid:**
- ObjectPool lifetime token + epoch; intentional late-drop vs UAF
- DRQ heap slots (stack overflow fixed)
- Ordered stop_workers / dtor (disarm → drain → join)
- `callbacks_armed_flag_` heap token for provider callbacks
- LocalBook `create_order` pool; Hybrid virtual `on_book_trades` / `sweep`
- QuestDB `unique_ptr` chain cold-path
- Parser `string_view` ephemeral into owned fields

**Structural cracks:**
1. **Funding dual-producer:** user-data thread calls `funding_event_factory_` → `acquire_pooled(funding_pool_)` and `event_publisher_` → `publish_event` while engine thread also publishes → violates EventRing SPSC and DRQ single-consumer.
2. **ControlBlockPool** deallocate uses raw pool pointer without lifetime/epoch (asymmetric vs ObjectPool).
3. **`in_use` not decremented** on intentional late-drop → MC rearm accounting risk.
4. Hybrid quote ladder still `make_shared<order>`.
5. `get_halt_flag()` mutable ref footgun (documented; `is_halted()` preferred).

---

## Detaillierte Findings

| ID | Sev | Location | Description | Source |
|----|-----|----------|-------------|--------|
| **H1** | HIGH | `engine.cpp:99-104`, `binance_futures_provider.h:412-432` | Live funding path: off-thread `acquire_pooled` + `publish_event` dual-produces SPSC EventRings and dual-drains DRQ | Arch agent + code verify |
| **H2** | HIGH | `control_block_pool.h:230-234` | CB `deallocate` always `release_slot` via raw `pool*`; no lifetime token — late drop after engine dtor can UAF CB free-list | Arch agent |
| **M1** | MEDIUM | `object_pool.h:163-170` | Intentional leak path skips `in_use_atomic_` decrement → false exhaustion under MC reuse | Arch agent |
| **M2** | MEDIUM | `hybrid_executor.h:171-179` | Synthetic quote reseed still `std::make_shared<order>` (strategy path uses pool) | Both agents |
| **M3** | MEDIUM | `engine.cpp:1718-1719`, adapters | Transient `vector<fill_event>` on every `process_adapter_fills` / poll | Arch agent |
| **M4** | MEDIUM | `engine.h:557-563` | Mutable `get_halt_flag()` still public; mid-run clear possible | Arch agent |
| **L1** | LOW | `test_hotpath_pool_prewarm.cpp:60` | ASan alloc ceiling 9000 flaky under dashboard wall-clock refresh | Tooling agent |
| **L2** | LOW | `object_pool.h:114-117` | DRQ full → mutex `push()` from workers (jitter) | Arch agent |
| **L3** | LOW | ObjectPool LateDrop* + LSan | 17 B intentional process-exit leak | Tooling agent |
| **L4** | LOW | Streaming `[&]` in `engine.cpp` | Safe while `run_streaming` synchronous; fragile if async | Arch agent |
| ~~H-prior~~ | FIXED | `deferred_return_queue.h` | Heap slots for DRQ | Prior check + code |
| ~~M-prior~~ | FIXED | Hybrid raw LocalBook ptr | Removed; virtual IExecutionAdapter | Residual PR |

---

## Phasenbasierter Remediation-Plan

**WICHTIG:** Für einen späteren Grok Build Agent **mit Schreibrechten**. Dieses memory-checks Run hat **keinen** Quellcode geändert.

### Phase 1: Vorbereitung & Setup

**Ziel:** Reproduzierbare Analyseumgebung und Baselines.

1. Cwd: `/home/leonard/work/projects/truetest/core`, branch from current `master`.
2. Configure with compile commands + sanitizers:

```bash
cmake --preset linux-asan   # ENABLE_BINANCE should be ON
# or:
cmake -B out/build/linux-asan -DBUILD_TESTS=ON -DENABLE_ASAN=ON -DENABLE_UBSAN=ON \
  -DENABLE_BINANCE=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build out/build/linux-asan -j"$(nproc)" --target truetest_tests
```

3. Baseline (expect: L2Burst absolute-count may fail under ASan; grow test must pass):

```bash
./out/build/linux-asan/truetest_tests \
  --gtest_filter='HotpathPoolPrewarm.*:HotpathAllocs.*:ObjectPool.*:DeferredReturnQueue.*:TickToTradeSafety.*'
./out/build/linux-tests/truetest_tests \
  --gtest_filter='HotpathPoolPrewarm.*:HotpathAllocs.*'
./scripts/check-hotpath-json.sh
./scripts/check-layer-deps.sh
./scripts/check-live-safety-freeze.sh
```

4. Optional static:

```bash
clang-tidy -p out/build/linux-asan src/engine/engine.cpp src/types/object_pool.h
```

**Abhängigkeiten:** none.  
**Verify:** binaries build; non-ASan hotpath 5/5 green.

---

### Phase 2: Kritische Sofort-Fixes (H1 dual-producer, H2 CB lifetime)

**Ziel:** Eliminieren direkter UAF / ring-corruption Potenziale.

#### Schritt 2.1 – H1: Funding / custom events fan-in to engine thread

**Problem:** User-data thread calls `publish_event` and `acquire_pooled` (`engine.cpp` wiring + `binance_futures_provider.h` funding handler).

**Konkrete Schritte:**

1. Introduce a sole-engine-thread ingress, e.g. SPSC or mutex-protected queue of `shared_ptr<event>` (or non-pooled funding DTO) owned by `engine`.
2. Change provider wiring so `set_event_publisher` **only enqueues**; engine loop drains before/after market handling and then calls existing `publish_event` / portfolio apply.
3. Change `set_funding_event_factory` so off-thread path does **not** call `acquire_pooled` — either:
   - construct a value DTO and let engine `acquire_pooled` when draining, or
   - enqueue prebuilt heap `funding_event` (cold path OK) without touching ObjectPool free-list from the WS thread.
4. Audit all other `set_event_publisher` / callback sites for the same pattern.

**Sketch (engine side):**

```cpp
// engine.h (illustrative)
std::mutex ingress_mu_;
std::vector<std::shared_ptr<event>> ingress_events_;

void enqueue_external_event(std::shared_ptr<event> ev) {
    std::lock_guard lock(ingress_mu_);
    ingress_events_.push_back(std::move(ev));
}

void drain_external_events(std::size_t& event_count) {
    std::vector<std::shared_ptr<event>> local;
    {
        std::lock_guard lock(ingress_mu_);
        local.swap(ingress_events_);
    }
    for (auto& ev : local) {
        // portfolio side-effects for funding, then publish_event on engine thread only
        publish_event(ev);
    }
}
```

**Publisher lambda becomes:**

```cpp
config_.provider->set_event_publisher(
    [this, armed_for_pub](std::shared_ptr<event> ev) {
        if (!armed_for_pub || !armed_for_pub->load(std::memory_order_acquire)) return;
        enqueue_external_event(std::move(ev));
    });
```

5. Call `drain_external_events` from streaming/history loops (same places as other drains).
6. Freeze-touching `engine.cpp` commits need:

```
LIVE_SAFETY_CCB_APPROVED
```

7. Verify:

```bash
# Prefer TSan after this change:
# cmake TSAN preset if available, or -DENABLE_TSAN=ON
./out/build/linux-asan/truetest_tests --gtest_filter='*TickToTradeSafety*:*LiveSafety*:*Funding*'
```

#### Schritt 2.2 – H2: ControlBlockPool lifetime token

**Problem:** `control_block_allocator::deallocate` always touches `pool_` (`control_block_pool.h`).

**Konkrete Schritte:**

1. Open `src/types/control_block_pool.h`.
2. Mirror ObjectPool: heap `std::shared_ptr<std::atomic<bool>> lifetime_` disarmed in dtor; optional epoch.
3. Allocator holds a **copy** of the lifetime token (shared_ptr).
4. In `deallocate`:

```cpp
void deallocate(T* p, std::size_t n) noexcept {
    (void)n;
    if (!lifetime_ || !lifetime_->load(std::memory_order_acquire) || !pool_) {
        // intentional leak of CB slot — prefer over UAF
        return;
    }
    pool_->release_slot(p);
}
```

5. Tests: extend `tests/test_*control*block*` or ObjectPool-style late-drop holding a ring `shared_ptr<event>` across engine destruction; expect no ASan UAF.
6. Rebuild ASan + full focused suite.

**Abhängigkeiten:** Phase 1. H1 and H2 independent; do H1 first if live funding is in use.

---

### Phase 3: Ownership & Lifetime Modernisierung (M1–M3)

**Ziel:** Accounting + pool hygiene; no hot-path inventiveness.

#### 3.1 – M1 `in_use` on intentional leak

In `object_pool.h` Returner else-branch, either:

```cpp
} else {
    // Still drop in_use so MC rearm accounting stays honest.
    if (pool)
        pool->in_use_atomic_.fetch_sub(1, std::memory_order_relaxed);
    // slot intentionally abandoned
}
```

**or** reset `in_use` inside `rearm_for_reuse` after documenting orphans, plus debug assert `in_use==0` after full drain before rearm.

Verify:

```bash
./out/build/linux-asan/truetest_tests --gtest_filter='ObjectPool.*'
```

#### 3.2 – M2 Hybrid quote pooling

In `hybrid_executor.h` `on_mid_price`, replace:

```cpp
auto t1 = book_->add_order(std::make_shared<order>(...));
```

with:

```cpp
auto t1 = book_->add_order(book_->create_order(
    ob_order_type::good_till_cancel, bid_id, side::buy,
    Price::from_double(bid_px), qty));
```

Keep `quote_ids_` cancel-only reseed. Run:

```bash
./out/build/linux-tests/truetest_tests --gtest_filter='HybridExecutor.*'
```

#### 3.3 – M3 fill staging (optional, measured)

- Member `std::vector<fill_event> fill_scratch_` on engine, cleared each poll; or
- Pre-reserve adapter `pending_fills_` capacity at startup.
- Measure with HotpathAllocs before claiming win.

---

### Phase 4: Concurrency & Atomics

**Ziel:** Prove sole-producer after Phase 2.

1. Build with TSan if preset exists; otherwise document gap.
2. Run worker/ring/funding-adjacent tests under TSan.
3. Grep audit: no second writer to EventRings.

```bash
rg -n "try_push|publish_event" src/engine/ src/providers/ --glob '*.{h,cpp}'
```

4. M4 halt API (optional, freeze-adjacent):

- Prefer production API: `is_halted()` + `trigger_halt` only.
- Tests: friend or `force_halt_for_test()`.
- Do not remove `start_workers` clear without redesigning multi-run.

---

### Phase 5: Hot-path alloc / test harness (L1, L3)

**Ziel:** Stable CI under ASan without greenwashing grow discipline.

#### 5.1 – L1 ceiling under ASan

Pick one:

**A)** Raise ceiling with comment (dashboard wall-clock under ASan):

```cpp
// tests/test_hotpath_pool_prewarm.cpp
EXPECT_LE(snap.count, 12000u) << "allocs=" << snap.count;  // ASan dashboard noise
```

**B)** Better: disable dashboard refresh during measure window (test-only hook / null builder).

**C)** Assert only `grow_count` / pool in_use (already partially done in same test for control_block_pool).

Keep sibling `L2BurstUsesPooledEvents_NoRuntimeGrow` as the **hard** zero-grow gate.

#### 5.2 – L3 intentional LSan

```bash
# tests-only suppressions file (example)
# leak:ObjectPool_LateDropNonTrivialAfterDtorIsSafe
# ASAN_OPTIONS=detect_leaks=1:suppressions=tests/lsan.supp
```

Do **not** call `~T` after pool death to silence LSan.

#### 5.3 – Hotpath verification commands (repo-canonical)

```bash
./out/build/linux-tests/truetest_tests --gtest_filter='*HotpathAllocs*:*HotpathPoolPrewarm*'
./out/build/linux-asan/truetest_tests --gtest_filter='*HotpathAllocs*:*HotpathPoolPrewarm*'
# Optional rebaseline when intentional cold-path changes:
# TRUETEST_REBASELINE_ALLOCS=1 ./out/build/linux-tests/truetest_tests --gtest_filter='*HotpathAllocs*'
```

---

### Phase 6: Verification & Re-Check

1. Full gates:

```bash
./scripts/check-hotpath-json.sh
./scripts/check-layer-deps.sh
./scripts/check-live-safety-freeze.sh
```

2. Full unit suite non-ASan + focused ASan.
3. Synthetic shadow smoke:

```bash
./out/build/linux-tests/engine_shadow \
  --provider synthetic --strategy sma --seed 424242 \
  --no-pin --status-format off --no-tui --output /tmp/shadow-mc.json
```

4. Re-run this skill:

```text
/memory-checks check-ups
```

5. Compare new dated report: **no open HIGH** for dual-producer / CB lifetime.
6. Freeze commits: `LIVE_SAFETY_CCB_APPROVED` on any `engine.cpp` / freeze-list touch.
7. Do **not** claim unattended live readiness solely from this check.

---

### Phase 7: Dokumentation & Suppression Management

1. Update `check-ups/` with delta after Phase 2–5.
2. Document intentional late-drop leak contract next to ObjectPool tests.
3. Document EventRing sole-producer policy in `docs/architecture/` or engine comments (funding fan-in).
4. If LSAN suppressions added: keep them **test-only**, never production binaries.
5. Note residual: 4h mainnet shadow soak still operator-owned (`2026-07-30-residual-risks-resolution.md`).

---

## Repo-critical areas coverage checklist

| Area | Covered |
|------|---------|
| `engine.cpp` / workers / halt | Yes |
| `ring_buffer` SPSC + sole-producer policy | Yes (H1) |
| `object_pool` + `deferred_return_queue` | Yes |
| `control_block_pool` | Yes (H2) |
| execution / LocalBook / Hybrid | Yes |
| QuestDB ownership | Yes (solid) |
| risk / binance futures funding | Yes (H1) |
| string_view / callback captures | Yes (L4, parsers OK) |
| HotpathAllocs / prewarm | Yes |

---

## Explicit non-claims

- This report does **not** claim TSan-clean production.
- This report does **not** claim 4h mainnet shadow soak.
- This report does **not** claim unattended live readiness.
- Intentional ObjectPool late-drop leaks are **by design** (prefer leak over UAF).

---

*Generated by memory-checks skill. Subagents: Tooling & Evidence + Ownership & Architectural Lifetime. No source files modified except this report.*
