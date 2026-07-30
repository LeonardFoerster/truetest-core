# Memory Safety Check – 2026-07-30

**Modus:** PR / Ad-hoc Feature (Bitget Futures Provider branch `provider`)  
**Analysierter Pfad:** Branch `provider` @ `ee17a89` vs merge-base `master` `73fb4ed`  
**Projekt-Root:** `/home/leonard/work/projects/truetest/core`  
**Report-Ziel:** `reports/memory-checks/2026-07-30-memory-check.md`  
**Skill:** `/memory-checks` — strikt read-only (nur dieser Report geschrieben)

---

## Zusammenfassung

| Severity | Count | Notes |
|----------|-------|-------|
| **HIGH** | 2 | WS cross-thread `ws_` access; private-WS re-open without join (Binance-parity patterns) |
| **MEDIUM** | 5 | Liveness raw pointer contract; bridge callbacks uncleared; shared REST serialization; halt-cb window; double-close opacity |
| **LOW / Info** | 6 | Test-harness LSan 17 B; clang-tidy naming; TSan not run; cppcheck missing; multi-trade defer; etc. |

**Gesamteinschätzung:**  
Unter **ASan+UBSan** sind die **Bitget-Unit-Tests (195/195) clean** (kein LSan). ExecutionBridge/Risk-Fokus (38/38) clean. Der einzige LSan-Hit (17 Bytes) stammt aus **`tests/helpers/alloc_counter.cpp`** (Hotpath-Filter) und ist **kein Bitget-Produktions-Leak**.

Ownership-/Lifetime-Analyse zeigt **keine Bitget-spezifische UAF-Katastrophe**; Thread-Join auf Happy Path ist solid. Die ernstesten Lifetime-Themen (**H1/H2**) sind **systemische Beast-Sync-Muster** (auch Binance), die Bitget 1:1 portiert. Vor Live-Kapital: **TSan** auf DMS/private WS + billige Hardening-Fixes (join-before-reopen, callback clear).

**Hot-path / pools:** ObjectPool `forbid_runtime_grow` und Hotpath-Alloc-Suite unter ASan grün. Bitget berührt Engine-Rings/QuestDB nicht.

---

## Tool-Ergebnisse

### Build (Sanitizer)

```bash
cmake -B build-asan \
  -DBUILD_TESTS=ON \
  -DENABLE_ASAN=ON \
  -DENABLE_UBSAN=ON \
  -DENABLE_BITGET=ON \
  -DENABLE_BINANCE=ON \
  -DENABLE_DEBUG=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-asan -j"$(nproc)" --target truetest_tests
# → Built target truetest_tests (exit 0)
```

### clang-tidy

```bash
clang-tidy -p build-asan src/providers/bitget/bitget_futures_register.cpp --quiet
```

| Finding | Severity |
|---------|----------|
| `readability-identifier-naming` on `_reg_bitget_futures` (line 152) | LOW style |

Kein Memory-/UB-Signal.

### cppcheck

**Not installed** on this host (`command -v cppcheck` empty). Coverage gap only.

### AddressSanitizer + LeakSanitizer

| Filter | Result | LSan |
|--------|--------|------|
| `*Bitget*` | **195/195 PASSED** | **No report** |
| `*Hotpath*:*ObjectPool*` | **26/26 PASSED** | **17-byte direct leak** → `tests/helpers/alloc_counter.cpp:39` `operator new` |
| `*ExecutionBridge*:*RiskManager*:*EngineVenue*` | **38/38 PASSED** | No LSan in tail |
| Combined Bitget+Hotpath+Bridge | All listed tests ok | Same 17-byte harness leak at process exit |

**LSan detail (Hotpath only):**
```
Direct leak of 17 byte(s) in 1 object(s) allocated from:
  #0 malloc (libasan)
  #1 operator new in tests/helpers/alloc_counter.cpp:39
SUMMARY: AddressSanitizer: 17 byte(s) leaked in 1 allocation(s).
```

**Interpretation:** Intentional alloc-counter override for hotpath tests. Reproduces **without** Bitget filter. **False positive w.r.t. Bitget production code.**

### ThreadSanitizer

**Not run** (PR mode). Ownership panel flags DMS + private/public WS multi-thread surfaces as TSan-priority.

### Weitere (Valgrind etc.)

Not run. ASan+LSan sufficient for unit-scope; Valgrind optional for long-run cold path.

### Hotpath / alloc_counter

- `*HotpathAllocs*`, `*HotpathAllocMatrix*`, `*HotpathPoolPrewarm*`, `*ObjectPool*` under ASan: **PASS**
- `ForbidRuntimeGrowThrowsWithoutGrowing` green
- Re-baseline note: `TRUETEST_REBASELINE_ALLOCS=1` exists for intentional alloc baseline updates (not used this run)

### Ownership greps (Bitget + bridge)

- Threads: DMS `[this]` + `join` on `stop()`; private WS `reader_` + `join` on `close()`
- Bridge: `fill_tx->set_on_message([this]...)` — not cleared on close
- Parser: `string_view` into caller buffers; results `.assign` into owned strings
- No raw `new`/`delete` in Bitget production paths (only OpenSSL free / `= delete`)

---

## Subagent-Befunde

### Subagent 1 – Tooling & Evidence

**ID:** `019fb2eb-6f7d-78d1-ae02-52c62597a5e4` (fresh)

- **No HIGH/MEDIUM sanitizer defects** in production Bitget from evidence.
- 17-byte leak = **test harness noise** (false positive for Bitget).
- clang-tidy naming only.
- **Production Bitget clean under ASan for unit tests: YES** (with residual: TSan/full suite/live E2E not run).
- Priority next: **TSan on DMS/private WS**, not ASan remediations on provider.

### Subagent 2 – Ownership & Architectural Lifetime

**ID:** `019fb2eb-6f7d-78d1-ae02-52d994fdf619` (fresh)

| ID | Severity | Summary |
|----|----------|---------|
| H1 | HIGH | Cross-thread `ws_` access (close vs reader) — Binance parity |
| H2 | HIGH | Private WS `open()` after error without join → `std::thread` assign can `terminate` |
| M1 | MEDIUM | Liveness raw `atomic*` into `dms_` — engine stop order protects; fragile contract |
| M2 | MEDIUM | Bridge `[this]` callbacks never revoked on close |
| M3 | MEDIUM | Double-close private WS (idempotent) |
| M4 | MEDIUM | Shared REST mutex can block DMS/kill behind long I/O |
| M5 | LOW–MED | Frame `string_view` valid only until next read (consumer OK today) |
| M6 | MEDIUM | Halt-cb wired after `open()` — reconnect window before halt |

**Verdict:** Bitget **not worse than Binance**; happy-path joins solid; systemic Beast/callback debts remain.

---

## Detaillierte Findings

### HIGH-01 — Cross-thread WebSocket stream access
- **Datei:** `src/providers/bitget/bitget_private_ws_transport.h:188–212` (close); reader uses `ws_` concurrently  
- **Auch:** `src/providers/bitget/bitget_transport.h` close vs `read_frame_blocking`  
- **Beschreibung:** `close()` may call `ws_->close()` while reader thread reads; concurrent stream access is UB.  
- **Quelle:** Ownership subagent H1; pattern matches Binance user-data transport.  
- **Schwere:** HIGH (data race / crash potential under concurrent close)

### HIGH-02 — Re-`open()` after error without joining prior reader
- **Datei:** `src/providers/bitget/bitget_private_ws_transport.h:168–185`  
- **Beschreibung:** After fatal path, `reader_` may still be joinable; `reader_ = std::thread(...)` invokes `std::terminate` if prior thread not joined.  
- **Quelle:** Ownership H2  
- **Schwere:** HIGH for recovery/tests; production often process-restart after halt

### MEDIUM-01 — Liveness raw pointer into DMS object
- **Datei:** `src/providers/bitget/bitget_futures_provider.h:447–460`; consumer `src/engine/engine.cpp:116–124`  
- **Beschreibung:** `last_alive_ms` points at atomic owned by `dms_`. Safe if engine always stops watchdog before provider close (current order). UAF if that invariant breaks.  
- **Quelle:** Ownership M1  
- **Parität:** Binance futures DMS

### MEDIUM-02 — ExecutionBridge fill callbacks hold `[this]` without clear
- **Datei:** `src/execution/execution_bridge.h:122–130`, `164–177`  
- **Beschreibung:** Callbacks set in ctor; `close()` joins transport but does not clear `set_on_message`/`set_on_status`. Safe today due to join order; fragile if fill_tx reused.  
- **Quelle:** Ownership M2; bridge modified this branch for dual-channel correctness

### MEDIUM-03 — Shared REST client can stall DMS/kill
- **Datei:** `src/providers/bitget/bitget_rest_client.h` (`connection_mu_`); DMS/kill/order share client  
- **Beschreibung:** Long/unbounded REST holds mutex; heartbeat/kill serialize behind it. Lifetime of client OK (shared_ptr); liveness risk.  
- **Quelle:** Ownership M4

### MEDIUM-04 — Halt callback installed after provider open
- **Datei:** Engine wiring after `provider->open()`; private WS reconnects until `fatal_cb_` set  
- **Beschreibung:** Window where disconnect reconnects instead of halt. Safety residual, not classic UAF.  
- **Quelle:** Ownership M6; pre-existing multi-venue pattern

### LOW-01 — Test harness LSan 17-byte leak
- **Datei:** `tests/helpers/alloc_counter.cpp:39`  
- **Beschreibung:** Hotpath suite under ASan reports 1 allocation leaked. Not Bitget production.  
- **Quelle:** Tooling subagent F1

### LOW-02 — clang-tidy register symbol naming
- **Datei:** `src/providers/bitget/bitget_futures_register.cpp:152`  
- **Beschreibung:** `_reg_bitget_futures` naming style.  
- **Quelle:** clang-tidy

### LOW-03 — TSan / full ASan suite / Valgrind / cppcheck not run
- Coverage gaps only.

### LOW-04 — Frame buffer `string_view` contract
- **Datei:** `bitget_transport.h` / `data_bridge` sync parse  
- **Beschreibung:** Views valid until next read; current consumers copy. Document/assert.

---

## Phasenbasierter Remediation-Plan

**WICHTIG:** Für einen späteren Grok-Build-Agenten **mit Schreibrechten**. Dieser Memory-Check-Run hat **keinen** Source-Code geändert.

### Phase 1: Vorbereitung & Setup

**Ziel:** Reproduzierbare Sanitizer-Umgebung und Baseline.

1. Projekt-Root: `/home/leonard/work/projects/truetest/core`
2. Branch: `provider` (oder PR-Branch mit Bitget)
3. Configure:
   ```bash
   cmake -B build-asan \
     -DBUILD_TESTS=ON \
     -DENABLE_ASAN=ON \
     -DENABLE_UBSAN=ON \
     -DENABLE_BITGET=ON \
     -DENABLE_BINANCE=ON \
     -DENABLE_DEBUG=ON \
     -DCMAKE_BUILD_TYPE=RelWithDebInfo
   cmake --build build-asan -j"$(nproc)" --target truetest_tests
   ```
4. Baseline (sollte grün bleiben):
   ```bash
   ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 \
     ./build-asan/truetest_tests --gtest_filter='*Bitget*'
   ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 \
     ./build-asan/truetest_tests --gtest_filter='*Hotpath*|*ObjectPool*'
   ```
5. Optional TSan-Build (separates Build-Dir, mutually exclusive with ASan in many setups):
   ```bash
   cmake -B build-tsan -DBUILD_TESTS=ON -DENABLE_TSAN=ON \
     -DENABLE_BITGET=ON -DENABLE_BINANCE=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
   cmake --build build-tsan -j"$(nproc)" --target truetest_tests
   ```

**Abhängigkeiten:** Keine.  
**Verify:** Bitget 195 pass; note any LSan only from alloc_counter on hotpath filter.

---

### Phase 2: Kritische Sofort-Fixes (UAF / terminate / races)

**Ziel:** HIGH-01 und HIGH-02 beheben (Bitget private + public WS; optional mirror Binance later).

#### Schritt 2.1 – HIGH-02: join before re-open (private WS)

**Datei:** `src/providers/bitget/bitget_private_ws_transport.h`

**Problem:** Assign to joinable `std::thread` after error path.

**Konkrete Schritte:**

1. Öffne `src/providers/bitget/bitget_private_ws_transport.h`
2. Am Anfang von `open()`, nach dem early-return für already-open, **vor** dem Spawn:

```cpp
// Ensure previous reader is fully reaped (error/fatal paths leave joinable threads).
if (reader_.joinable())
    reader_.join();
```

3. In `close()`, `ws_->close()` unter `ws_mu_` halten (siehe Schritt 2.2).
4. Nach join: Callbacks optional clearen:
```cpp
message_cb_ = {};
status_cb_ = {};
```

5. Test: Unit-Test der `open()` nach simulated error state (force `state_=error` with joinable thread) — or extend existing transport tests if inject seam exists.
6. Verifizieren:
   ```bash
   cmake --build build-asan -j --target truetest_tests
   ASAN_OPTIONS=detect_leaks=1 ./build-asan/truetest_tests --gtest_filter='*Bitget*'
   ```

#### Schritt 2.2 – HIGH-01: serialize `ws_` access

**Datei:** `src/providers/bitget/bitget_private_ws_transport.h` (+ analog public `bitget_transport.h` wenn foreign-thread close)

**Empfohlene Policy (minimal):**

1. Alle Zugriffe auf `ws_` (read, write, close, reset) unter demselben Mutex **oder**
2. Close-from-other-thread nur: `stop_flag_` + `shutdown()` auf native socket, Reader-Thread allein `ws_.reset()`.

**Beispiel-Skizze `close()`:**

```cpp
void close() override
{
    stop_flag_.store(true);
    cv_.notify_all();
    {
        std::lock_guard<std::mutex> lk(ws_mu_);
        if (ws_) {
            beast::error_code ec;
            try { ws_->close(websocket::close_code::normal, ec); } catch (...) {}
        }
    }
    if (reader_.joinable())
        reader_.join();
    {
        std::lock_guard<std::mutex> lk(ws_mu_);
        ws_.reset();
        ioc_.restart();
    }
    set_state(lifecycle::closed, "closed");
}
```

3. Reader-Pfade, die `ws_` nutzen, müssen denselben Lock halten **oder** document that only reader owns stream after open and close only interrupts via flag+socket shutdown.

4. TSan:
   ```bash
   ./build-tsan/truetest_tests --gtest_filter='*BitgetFuturesDeadMans*:*Bitget*Transport*'
   ```

**Abhängigkeiten:** Phase 1.  
**Verify:** ASan green; TSan no race on private WS close-vs-read.

---

### Phase 3: Ownership & Lifetime Modernisierung

**Ziel:** MEDIUM callback/liveness hardening ohne API-Bruch wo möglich.

#### 3.1 – ExecutionBridge: revoke fill callbacks on close

**Datei:** `src/execution/execution_bridge.h`

In `close()`, vor/nach `fill_tx->close()`:

```cpp
if (d_.fill_tx) {
    d_.fill_tx->set_on_message({});
    d_.fill_tx->set_on_status({});
    d_.fill_tx->close();
}
```

**Tests:** Existing `ExecutionBridge` suite + `CloseClearsHandler` patterns; ensure no UAF if fill_tx shared.

**Note:** `execution_bridge.h` is freeze-**adjacent** (not freeze-10). Still review carefully; no `LIVE_SAFETY_CCB_APPROVED` required by freeze script unless freeze list later expands.

#### 3.2 – Document / enforce liveness lifetime

**Dateien:** `src/providers/provider.h` comment; `bitget_futures_provider.h` `get_liveness_sources`

1. Correct comment: atomic lives **inside** provider/DMS; **Watchdog must stop before provider destruction** (engine already does this at `engine.cpp` shutdown).
2. Optional future API: `shared_ptr<atomic<int64_t>>` beat cell (larger change).

#### 3.3 – REST per-call timeout default for live

**Datei:** `bitget_futures_provider.h` `open_live_path` after `rest_` create:

```cpp
rest_->set_per_call_timeout(std::chrono::milliseconds(3000));
```

So DMS/kill cannot block forever behind unbounded I/O (kill-switch already sets timeout per call).

**Abhängigkeiten:** Phase 2 preferred first.  
**Verify:** Bitget + ExecutionBridge ASan tests.

---

### Phase 4: Concurrency & Atomics (TSan)

**Ziel:** Data races formal beweisen/widerlegen.

1. Build `build-tsan` as in Phase 1.
2. Run:
   ```bash
   ./build-tsan/truetest_tests --gtest_filter='*BitgetFuturesDeadMans*:*BitgetFuturesKill*:*ExecutionBridge*'
   ```
3. If races on `ws_`: complete Phase 2.2.
4. DMS `last_beat_ms_` should be race-free (atomic). Confirm no non-atomic shared mutable without lock.

**Optional:** Concurrent stress test: open private WS mock + close from main thread while reader loops (injectable).

---

### Phase 5: Hot-path alloc / pool verification

**Ziel:** Keine Regression an Engine-Hotpath durch Bitget (Bitget sollte hier unberührt bleiben).

```bash
./build-asan/truetest_tests --gtest_filter='*HotpathAllocs*'
./build-asan/truetest_tests --gtest_filter='*HotpathAllocMatrix*'
./build-asan/truetest_tests --gtest_filter='*HotpathPoolPrewarm*'
./scripts/check-hotpath-json.sh
./scripts/check-layer-deps.sh
./scripts/check-live-safety-freeze.sh
```

If alloc baselines intentionally change: `TRUETEST_REBASELINE_ALLOCS=1` only with human review.

**Bitget-specific:** Market parse uses needle-scan + `symbol.assign` (Binance parity). No nlohmann. Multi-trade batch (`parse_all_trades`) not on production combined path — keep off hot path until pre-sized buffers exist.

---

### Phase 6: Verification & Re-Check

1. Rebuild ASan+UBSan full relevant suite:
   ```bash
   cmake --build build-asan -j --target truetest_tests
   ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
     ./build-asan/truetest_tests --gtest_filter='*Bitget*:*Hotpath*:*ExecutionBridge*:*ObjectPool*'
   ```
2. TSan suite (Phase 4).
3. Gate scripts (Phase 5).
4. Re-run this skill:
   ```text
   /memory-checks reports/memory-checks
   ```
5. Compare new report to `reports/memory-checks/2026-07-30-memory-check.md`.
6. Exit criteria: **no HIGH findings**; MEDIUM either fixed or explicitly accepted with owner.

---

### Phase 7: Dokumentation & Suppression Management

1. If LSan noise from `alloc_counter` pollutes CI: document suppression **only** for `tests/helpers/alloc_counter.cpp` (never production).
2. Ops: dual-channel residual already in `docs/operations/03-bitget-demo.md` — keep after disconnect drills.
3. Optional: install cppcheck for weekly runs.
4. **Do not** claim TSan-clean or mainnet memory-proven until Phase 4–6 complete.
5. Freeze-10: Bitget files not frozen; if `execution_bridge.h` callback clear lands, treat as freeze-adjacent review.

---

### Phase 8: QuestDB / Engine workers (repo-critical, not Bitget-touched)

**Ziel:** Weekly-style residual for areas Bitget did not modify but skill requires coverage note.

| Area | Status this PR |
|------|----------------|
| `src/engine/*worker*` | Unchanged; no new findings from Bitget |
| `src/threading/ring_buffer.h` | Unchanged; SPSC sole-producer intact |
| `src/types/object_pool.h` | Unchanged; hotpath ForbidRuntimeGrow green under ASan |
| `src/data/questdb/*` | Unchanged / not linked by Bitget |
| `src/risk/*` + Binance freeze safety | Unchanged |

**No remediation required for Bitget PR** on these unless weekly full-repo audit finds separate issues.

---

## Appendix A — Changed files (merge-base..HEAD)

```
src/providers/bitget/* (new)
src/execution/execution_bridge.h
src/bin/main.inc
cmake/*, CMakeLists.txt
docs/operations/03-bitget-demo.md
tests/providers/bitget/test_bitget_*.cpp, test_cli.cpp, test_execution_bridge.cpp
```

## Appendix B — Commands used this run

```bash
date +%Y-%m-%d   # 2026-07-30
cmake -B build-asan -DENABLE_ASAN=ON -DENABLE_UBSAN=ON -DENABLE_BITGET=ON -DENABLE_BINANCE=ON -DBUILD_TESTS=ON -DENABLE_DEBUG=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-asan -j --target truetest_tests
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 ./build-asan/truetest_tests --gtest_filter='*Bitget*'
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 ./build-asan/truetest_tests --gtest_filter='*Hotpath*:*ObjectPool*'
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 ./build-asan/truetest_tests --gtest_filter='*ExecutionBridge*:*RiskManager*:*EngineVenue*'
clang-tidy -p build-asan src/providers/bitget/bitget_futures_register.cpp --quiet
```

## Appendix C — Git hygiene (skill constraint)

This skill run writes **only** this report file. No source edits, no commits.

Verify after write:

```bash
git status --porcelain | head
# Expect report under reports/memory-checks/ as untracked or intended artifact only
```

---

## Remediation Status (2026-07-30 — implemented)

All HIGH/MEDIUM findings from this report were addressed on branch `provider`. Parallel implementers + integrated verification.

| ID | Status | Fix summary |
|----|--------|-------------|
| **HIGH-01** | **FIXED** | Private/public/combined WS: foreign-thread `close()` interrupts via lowest-layer `cancel`+`close` only; protocol `ws_->close()` only after reader join (private) or omitted on foreign close (public/combined) |
| **HIGH-02** | **FIXED** | `BitgetPrivateWsTransport::open()` joins joinable `reader_` before re-spawn; avoids `std::terminate` |
| **MEDIUM-01** | **FIXED** (docs) | `provider.h` + Bitget `get_liveness_sources()` document raw-pointer lifetime; watchdog must stop before provider teardown |
| **MEDIUM-02** | **FIXED** | `ExecutionBridge::close()` clears `set_on_message`/`set_on_status` before `fill_tx->close()`; test `CloseClearsFillCallbacks` |
| **MEDIUM-03** | **FIXED** | Live `open_live_path()` sets `rest_->set_per_call_timeout(3s)` after REST create |
| **MEDIUM-04** | **FIXED** | Live `apply_halt_cb_to_transports()` installs provisional fail-closed fatal (log-only) when engine halt not yet wired; paper/shadow leave unset for reconnect |
| **LOW-01** | **ACCEPTED** | 17 B LSan = harness `alloc_counter` override; comment only, no production suppression |
| **LOW-02** | **FIXED** | `_reg_bitget_futures` → `k_reg_bitget_futures` |
| **LOW-03** | **PARTIAL** | TSan suite run on Bitget+bridge (see below); Valgrind/cppcheck still optional |
| **LOW-04** | **FIXED** | Frame `string_view` lifetime documented on private/public/combined read paths |

### Files touched (remediation)

```
src/providers/bitget/bitget_private_ws_transport.h
src/providers/bitget/bitget_transport.h
src/providers/bitget/bitget_combined_transport.h
src/providers/bitget/bitget_futures_provider.h
src/providers/bitget/bitget_futures_register.cpp
src/execution/execution_bridge.h
src/providers/provider.h
tests/test_execution_bridge.cpp
tests/helpers/alloc_counter.cpp
```

### Verification evidence (post-fix)

| Check | Result |
|-------|--------|
| `check-hotpath-json.sh` | OK |
| `check-layer-deps.sh` | OK |
| `check-live-safety-freeze.sh` | OK (no freeze-list edits) |
| ASan `*Bitget*:*Hotpath*:*ExecutionBridge*:*ObjectPool*` | **248 PASSED** (17 B LSan harness only) |
| ASan `*HotpathAllocs*:*HotpathAllocMatrix*:*HotpathPoolPrewarm*` | **11 PASSED** |
| TSan `*Bitget*` | **195 PASSED**, no TSan report |
| TSan `*BitgetFuturesDeadMans*:*BitgetFuturesKill*:*ExecutionBridge*:*Bitget*Transport*` | **83 PASSED**, no TSan report |

### Residual (accepted / out of scope)

1. **Provisional halt is log-only** until engine wires real `set_halt_callback` — fail-closed (no reconnect), not full kill-switch path.
2. **Shared REST mutex** still serializes DMS/kill behind other calls; bound is now ~3s not unbounded.
3. **Liveness raw pointer** remains contract-based (documented); no `shared_ptr` beat cell.
4. **Binance parity** for Beast close-vs-read / join-before-reopen / 3s REST default not applied in this pass (Bitget-scoped).
5. **cppcheck / Valgrind** not installed/run.

**Exit criteria:** no open HIGH findings; MEDIUMs fixed or documented-accepted. Remediation complete for this report's action items.

---

*End of report — 2026-07-30 memory-check (Bitget provider PR mode).*
*Remediation implemented and verified same day.*
