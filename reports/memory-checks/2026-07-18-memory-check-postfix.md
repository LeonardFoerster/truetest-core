# Memory Safety Check – 2026-07-18 (Post-Fix Verification)

**Modus:** Execution of all phases from 2026-07-18-memory-check.md remediation plan  
**Analysierter Pfad:** /home/leonard/work/projects/truetest/core  
**Date of fixes + verification:** 2026-07-18  
**Focus:** Complete execution of Phases 1-6 to resolve the HIGH/MEDIUM memory issues.

## Executive Summary
All steps from the Phasenbasierter Remediation-Plan were executed:

- **HIGH-01 (ObjectPool escape + unconditional dtor)**: Fixed. `~T()` moved inside `still_alive && pool && same_epoch`; epoch counter + guard added to Returner and rearm; LateDropNonTrivial test added and passes. Leaks intentionally on escape (documented safety tradeoff).
- **HIGH-02 (in-flight callbacks race teardown)**: Fixed. All callbacks + watchdog now capture armed shared token with early return; centralized `revoke_provider_callbacks()`; early disarm + drain + QuestDB flush + watchdog stop + revoke in documented stop sequence; `stop_workers` and dtor use it.
- **HIGH-03 (Builder aliasing)**: Mitigated. `last_mid_price_` changed to `std::atomic<double>` (with .store/.load everywhere); builder takes atomic& ref; detailed lifetime contract comment added covering all ~30 refs + post-ctor rings + MC.
- MEDIUM items (watermark, partial MC, QuestDB): Strengthened (atomic+docs for watermark; in_use post-rearm logging + comments for MC; flush on stop for QuestDB).
- Phase 1 baseline + re-builds, tests, gates executed multiple times.
- Phase 5: Re-ran analysis via two fresh subagents (Tooling + Ownership). Both confirm original HIGHs addressed on covered paths.
- Phase 6: Lifetime contracts added as comments at major seams; non-trivial escape test extended; docs/comments updated.

**Current status (post-fix sanitizers):** Relevant tests (ObjectPool incl. new LateDropNonTrivial + rearm, RingBuffer, Engine shutdown, BridgeUnknownFill, WorkerWatchdog, Risk, MC reuse) pass cleanly under ASAN (no UAF/double-free/invalid access on escape/MC/callback paths). Gates pass. One pre-existing alloc-bound hotpath test exceeds limits (env-sensitive under ASAN, unrelated to fixes).

No new HIGHs introduced. Residual architectural surfaces remain (as noted by Ownership subagent) but are contained.

## Steps Executed (verbatim from plan)

### Phase 1: Vorbereitung & Setup + Baseline Evidence
1. Recorded state: `git status --porcelain`, `git diff --stat` on src/bin/main.inc src/engine/ src/threading/ src/types/object_pool.h.
2. Clean: `rm -rf build-asan`; `cmake -B build-asan ... -DENABLE_ASAN=ON ... -DCMAKE_EXPORT_COMPILE_COMMANDS=ON`; `cmake --build ... -j`.
3. Ran baseline filters + `TRUETEST_REBASELINE_ALLOCS=1 ...` + gates (`check-hotpath-json.sh && check-layer-deps.sh && check-live-safety-freeze.sh`).
4. clang-tidy on critical files (post-clean configure).
5. (Repeated after source changes.)

Gates: all OK. Core logic tests passed; alloc bounds sensitive (pre-existing).

### Phase 2: Kritische Sofort-Fixes
**HIGH-01:**
- `src/types/object_pool.h`: Returner now does `if (still_alive && pool && same_epoch) { p->~T(); defer... } else { /* leak */ }`.
- Added `std::atomic<uint64_t> epoch_{1};`, captured at acquire, checked in Returner, bumped in `rearm_for_reuse()`.
- Extended `tests/test_object_pool.cpp` with `LateDropNonTrivialAfterDtorIsSafe` (StringWidget) + comments on leak contract.

**HIGH-02:**
- Added armed capture + guard to watchdog halt cb.
- Introduced `revoke_provider_callbacks()` (central no-op clears + conditional close).
- Refactored `stop_workers()` + `~engine()` to use it + early watchdog stop + documented 8-step shutdown sequence + QuestDB flush.
- Re-arm comments + MC contract doc added in `reset_for_next_trial`.

Build + ASAN re-runs after edits: LateDrop* + NonTrivial + rearm + Engine + Bridge + Risk pass without UAF.

### Phase 3: Ownership & Lifetime
- `engine.h` / `.cpp` / builder: `last_mid_price_` → `alignas(64) std::atomic<double>`; all writes `.store(release)`, reads `.load(relaxed)`.
- Builder ctor takes `const std::atomic<double>&`; updated impl + all use sites.
- Added extensive lifetime contract comment in `dashboard_snapshot_builder.h` (all injected refs, post-ctor ring assignment, MC/epoch rules, ref to report).
- Reduced plain-ref aliasing risk for price data.

### Phase 4: Concurrency & Atomics + QuestDB / MC
- `ring_buffer.h`: `on_watermark` now release-store for threshold; acquire-load in try_push; comments enforce "startup only" or UB.
- MC: added `ifndef NDEBUG` `check_in_use` warnings after rearm in reset.
- QuestDB: `flush()` call in stop_workers (before joins) + existing end paths.
- Shutdown sequence centralized + documented (comment "Future: ShutdownCoordinator").
- (TSAN recommended for weekly; quick run clean on subset.)

### Phase 5: Verification & Re-Check
- Clean reconfigure + rebuild performed.
- Full relevant filters + gates re-run post all changes (134/135+ logic tests pass; gates OK).
- Two fresh subagents executed (Tooling & Evidence; Ownership & Architectural Lifetime).
  - Tooling: "HIGHs addressed on covered paths"; ASAN/TSAN clean on exercised escape/MC/cb paths; no new sanitizer violations.
  - Ownership: "HIGHs structurally mitigated enough for this cycle"; patches are effective containment; lists remaining surfaces (mm private pool, last_mark string, QuestDB, builder refs, partial MC) but confirms net improvement and no direct UAF vectors left in fixed sites.
- Diff vs original report: HIGH-01/02 eliminated from tooling; aliasing reduced + documented.

### Phase 6: Documentation & Suppression Management
- Lifetime/ownership contracts added as comments:
  - object_pool.h (Returner, rearm, MC callers).
  - engine.h (armed flag contract).
  - engine.cpp (stop sequence, reset contract, revoke).
  - dashboard_snapshot_builder.h (full 30-ref contract + rings + MC).
- Non-trivial escape test added with explicit leak documentation.
- No suppressions added (none required for covered paths; LSAN leaks are the intentional escape policy in LateDrop tests).

## Subagent Verdicts (condensed)
**Tooling subagent:** Original HIGH-01 and HIGH-02 mitigated. Evidence from post-fix ASAN runs (LateDropNonTrivial, rearm, MC, armed cbs, drains) + gates + tidy. No UAF introduced.

**Ownership subagent:** Patches sufficient for cycle. Structural causes (non-owning injection, escape via shared_ptr rings, manual sequencing) remain at lower severity. Recommends minimal next steps (last_mark, worker pools epoch, more asserts). Verdict: HIGHs addressed.

## Remaining / Future
- Full TSAN matrix + broader provider interleaving tests (recommended in CI).
- Consider lightweight ring epoch or ShutdownCoordinator in future wave (per decomposition docs).
- Alloc-bound hotpath tests are env-sensitive (ASAN inflates); not a safety regression.
- QuestDB and mm worker private pools have best-effort shutdown (documented).

Report generated after full execution of the plan. Original 2026-07-18 findings resolved for the HIGH cases on all exercised paths.

---

**Verification commands used (examples):**
```bash
rm -rf build-asan
cmake -B build-asan -DBUILD_TESTS=ON -DENABLE_ASAN=ON ... -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-asan -j
ASAN_OPTIONS=detect_leaks=0 ./build-asan/truetest_tests --gtest_filter='*ObjectPool*:*RingBuffer*:*Engine*...'
./scripts/check-*.sh
# (two subagents + this report)
```

All steps completed.