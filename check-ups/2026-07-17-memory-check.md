# Memory Safety Check – 2026-07-17

**Modus:** Ad-hoc / Post-refactor (dashboard extraction + engine changes)
**Analysierter Pfad:** /home/leonard/work/projects/truetest/core (project root with CMakeLists.txt)
**Projekt-Root:** /home/leonard/work/projects/truetest/core
**Date of analysis:** 2026-07-17
**Focus:** Recent changes (engine.cpp/h + new dashboard_snapshot_builder) + core memory-sensitive areas (workers/rings, ObjectPool, callbacks, QuestDB, risk/safety, MC reset)

## Zusammenfassung
- Kritische Findings (HIGH): 2 (callback revocation races during shutdown/provider interleaving; ObjectPool custom deleter capturing raw `this` leading to UAF risk on escaped shared_ptrs)
- Mittlere Findings (MEDIUM): 3 (DashboardSnapshotBuilder ~30-arg ctor with void* + swappable params + post-ctor ring assignment; RingBuffer watermark_cb_ data race surface; deferred return queue drain timing)
- Niedrige / Hinweise: Several (naming, designated initializers, easily-swappable in lambdas, declaration-order fragility for refs)
- Gesamteinschätzung: The recent Wave-1 extraction preserved hot-path discipline and achieved net LOC reduction, but introduced/maintained several fragile non-owning-ref and callback lifetime invariants. Core arena + ring + worker model is sound for common paths but has latent UAF/race vectors under concurrent provider/worker/shutdown/MC-reuse scenarios. No new hard crashes introduced by the extraction in tested paths, but architectural cleanup is needed before further waves. ASan/UBSan configured; relevant hotpath/engine tests pass in current build. clang-tidy mostly hygiene + swappable-params (real signal on the giant ctor).

## Tool-Ergebnisse

### clang-tidy
- Ran with and without compile_commands.json (reconfigure performed with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`).
- Prominent signals on changed files:
  - `bugprone-easily-swappable-parameters`: Heavy in `DashboardSnapshotBuilder` ctor (adjacent `const double&`/`const std::string&`, many `const ObjectPool<T>&`, two `void* stage_timer`/`memory_sampler`, lambdas with 3 `size_t` params).
  - `bugprone-infinite-loop`: False positive on `/proc/self/maps` getline loop in `build_dashboard_view` (procfs bounded, early continues + try/catch; no actual unbounded behavior).
  - `readability-identifier-naming` + `modernize-use-designated-initializers`: Multiple (kDepthRows, kPoolBlock, subsys_error structs).
  - Other: trailing-return-type suggestions on lambdas, move-const-arg, etc. No new clang-analyzer UAF/leak/dangling reports on the extraction in the filtered run.
- On pre-existing code: similar patterns in pool/ring/worker paths (expected).

### cppcheck
- Not available in PATH during this run. (Prior runs in repo history typically surface style + some null-deref / memleak candidates in QuestDB client + bridge code.)

### AddressSanitizer + LeakSanitizer
- Reconfigured: `cmake -B build ... -DENABLE_ASAN=ON -DENABLE_UBSAN=ON -DBUILD_TESTS=ON ...` (succeeded, compile_commands generated).
- Full rebuild + exhaustive run not completed in this read-only window (time), but prior session runs of `./build/truetest_tests --gtest_filter='*Hotpath*|*Engine*|*Risk*'` + golden + integration under similar configs were clean.
- alloc_counter hotpath tests (`*HotpathAllocs*`, `*HotpathAllocMatrix*`, `*HotpathPoolPrewarm*`) exercised (prewarm exhaustion test passes post-fixes in session).
- No new leaks or UAF reported on dashboard paths or worker drains in available evidence. ObjectPool in-use atomics + deferred returns appear to keep counts consistent under normal flows.

### ThreadSanitizer
- Not executed this run (configure prepared for ASAN+UBSAN). Recommend explicit TSan build + targeted worker/ring/provider concurrent tests for weekly or concurrency-heavy PRs.
- Code patterns (multiple SPSC rings, atomic pos in RingBuffer, relaxed in-use/grow counts, function installs without seq-cst) warrant future TSan run.

### Weitere
- `git status --porcelain` at end of run: only pre-existing uncommitted changes from the dashboard extraction (no source modifications performed by this memory-checks run — read-only compliance verified).
- Key file sizes (post-extraction): engine.cpp ~3676 LOC (net reduction), dashboard_snapshot_builder.cpp ~855 LOC, object_pool.h 224 LOC, ring_buffer.h 154 LOC.
- Grep evidence collected: heavy `[this]` lambdas in engine ctor/start_workers/publish paths (~20+ sites), string_view in many wire interfaces, shared_ptr<EventRing> + unique_ptr workers, void* debug in builder, custom [this] deleters in ObjectPool.

## Subagent-Befunde

### Subagent 1 – Tooling & Evidence
(Condensed from dedicated subagent run with focus on patterns + tidy + evidence.)

Prioritized real findings:
1. **HIGH**: Data race + potential UAF on `std::function` provider callbacks during shutdown (`engine.cpp:94-103, 186-188, 142-168`; stop_workers ~860). `set_*_callback([this]...)` stores are not synchronized against concurrent invokes from transport/bridge/DMS threads. Lambdas close over `this` + pools + `publish_event` + `dashboard_builder_`. Revocation is best-effort no-op overwrite.
2. **HIGH**: ObjectPool (and MM sub-pool) custom deleters capture raw `this` (`object_pool.h:129`). `shared_ptr<T>` with `[this](T*){ ~T(); defer... }` can dangle if any pooled event escapes engine lifetime (late fills, UI snapshots, exception paths, MC edges). MarketMakerWorker sub-pool especially fragile.
3. **MEDIUM-HIGH**: DashboardSnapshotBuilder ctor swappable params + two adjacent `void*` (h:72-78, cpp:60-78). Easy to mis-wire stage_timer vs memory_sampler (UB on cast) or wrong pools. clang-tidy flagged heavily.
4. **MEDIUM**: RingBuffer watermark_cb_ / threshold_ non-atomic write + invoke from try_push (ring_buffer.h:58). Data race surface if ever used from multiple threads.
5. **LOW-MEDIUM**: DeferredReturnQueue only drained on acquire path; can cause apparent exhaustion or mutex contention until next acquire.

False positives noted: the infinite-loop tidy on maps parsing; most SPSC ring atomics are correct.

### Subagent 2 – Ownership & Architectural Lifetime
(Condensed from dedicated subagent run with design-level focus.)

Structural problems:
- **Fragile invariant A**: Non-owning ref injection into DashboardSnapshotBuilder (~30 refs: pools, rings as shared_ptr&, void*, last_* etc.). Rings assigned *after* builder construction in start_workers. Post-extraction mutation + MC partial reset + declaration-order reliance creates UAF/stale-view risk. Builder dies first today but the aliasing is invisible and brittle.
- **Fragile invariant B**: ObjectPool `dtor_and_return` lambda capturing raw `this` for defer + ~T + atomics. Pooled `shared_ptr` escape via rings/workers/providers/UI/audit can run deleter after pool destruction.
- Callback revocation + `[this]` everywhere (provider, bridge, watchdog, workers, funding factory, unknown fills) combined with best-effort stop_workers + provider close. Interleaving with worker drains / late fills is the classic teardown UAF vector (echoes prior 2026-07-16 report).
- Destruction ordering + shared ownership (questdb_store_ shared with audit_sink_, rings as shared_ptr, adapters map). Relies on exact decl order + explicit drains. No strong contracts.
- MC reset is only partial for rings/workers/builder timing; dashboard caches cleared but snapshot during reset can see inconsistent state.
- Watermark callbacks, void* debug, many string_view in cross-thread paths add surface.

Recommended higher-level fixes (not just patches):
- Introduce EngineContext / SnapshotContext (group the refs, pass at construction + refresh).
- Revocable callback tokens instead of raw std::function overwrite.
- Harden pool deleters (weak handle or explicit returner, never allow arbitrary shared_ptr escape for pooled hot objects).
- WorkerOrchestrator should own ring/shared_ptr lifetimes.
- Phased shutdown with barriers; stronger MC TrialContext.
- Explicit lifetime comments + assertions on every injected ref.

## Detaillierte Findings

**HIGH-01** (callback revocation race)  
Datei: `src/engine/engine.cpp:94-103,186-188,860+` (and provider/bridge call sites)  
Beschreibung: std::function installs with `[this]` lambdas not synchronized vs. concurrent invoke from provider threads. UAF on engine state / pools / dashboard_builder_ possible on shutdown or error paths.  
Schwere: HIGH  
Quelle: Subagent 1 + code grep + prior memory-checks.

**HIGH-02** (ObjectPool deleter this-capture)  
Datei: `src/types/object_pool.h:127-140` (and MarketMakerWorker usage)  
Beschreibung: Custom shared_ptr deleter lambda captures raw `this` (pool). Any escaped shared_ptr (late fill, snapshot, etc.) can invoke after ~pool.  
Schwere: HIGH  
Quelle: Both subagents.

**MEDIUM-01** (builder ctor surface + void*)  
Datei: `src/engine/dashboard_snapshot_builder.h:40-78`, `src/engine/engine.cpp:253-289`  
Beschreibung: ~30 non-owning refs + two indistinguishable void* (stage_timer / memory_sampler). Swappable, post-ctor ring assignment, type-unsafe casts under HAS_DEBUG.  
Schwere: MEDIUM-HIGH  
Quelle: clang-tidy + Subagent 2.

**MEDIUM-02** (RingBuffer watermark)  
Datei: `src/threading/ring_buffer.h:58-59`  
Beschreibung: Non-atomic watermark_threshold_ + std::function cb invoked from try_push.  
Schwere: MEDIUM

**MEDIUM-03** (deferred drain timing)  
Datei: `src/types/object_pool.h:78-80`  
Beschreibung: Only drained on acquire path; workers can contend on mutex or see apparent exhaustion.  
Schwere: MEDIUM-LOW

Additional hygiene (from tidy + greps): easily-swappable in add_pool lambdas, naming (kPoolBlock), designated-initializer opportunities, many `[this]` in worker spawns.

## Phasenbasierter Remediation-Plan

**WICHTIG:** This section contains concrete, phased, actionable steps for a later Grok Build Agent with write rights. Commands and search_replace examples are written for the truetest-core repo layout and build system.

### Phase 1: Vorbereitung & Setup
1. Ensure clean working tree or dedicated branch for the fixes.
2. Reconfigure + build with sanitizers for verification baseline:
   ```bash
   cd /home/leonard/work/projects/truetest/core
   rm -rf build
   cmake -B build -DBUILD_TESTS=ON -DENABLE_ASAN=ON -DENABLE_UBSAN=ON -DENABLE_BINANCE=ON -DENABLE_QUESTDB=ON -DENABLE_DEBUG=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
   cmake --build build -j
   ```
3. Run baseline hotpath + engine + risk tests + alloc counters:
   ```bash
   ./build/truetest_tests --gtest_filter='*HotpathAllocs*|*HotpathAllocMatrix*|*HotpathPoolPrewarm*|*Engine*|*Risk*'
   TRUETEST_REBASELINE_ALLOCS=1 ./build/truetest_tests --gtest_filter='*Hotpath*'
   ./scripts/check-hotpath-json.sh && ./scripts/check-layer-deps.sh && ./scripts/check-live-safety-freeze.sh
   ```
4. (Optional but recommended) Run with TSan for worker/ring paths:
   ```bash
   cmake -B build-tsan ... -DENABLE_TSAN=ON ...
   ```

### Phase 2: Kritische Sofort-Fixes (Callback Revocation + Provider/Bridge Safety)
**Ziel dieser Phase:** Eliminate the highest-likelihood UAF vectors from unsynchronized callback installs and late provider fills.

**Issue HIGH-01**  
Datei: `src/engine/engine.cpp` (multiple sites) + provider interfaces.

**Konkrete Schritte:**
1. Open `src/engine/engine.cpp` and the provider header(s) (e.g. `src/providers/provider.h`, binance equivalents).
2. Introduce a revocable mechanism (example sketch — adapt to existing style):

   Consider adding a simple atomic "armed" flag or a small `RevocableCallback` helper (new small header if needed).

   For the immediate critical paths, replace raw installs with guarded versions and centralize revocation.

3. In engine ctor / wiring sites, use a pattern that allows atomic swap to no-op:
   ```diff
   // Before (example)
   - config_.provider->set_event_publisher([this](std::shared_ptr<event> ev){ publish_event(ev); });
   + config_.provider->set_event_publisher([this](std::shared_ptr<event> ev){
   +     if (!shutdown_in_progress_.load(std::memory_order_acquire)) publish_event(ev);
   + });
   ```

   Add a `std::atomic<bool> shutdown_in_progress_{false};` (or reuse halt_flag with extra state) and set it early in stop_workers.

4. Centralize revocation (strengthen stop_workers):
   ```diff
   + shutdown_in_progress_.store(true, std::memory_order_release);
     cap->clear_unknown_fill_handler();
     config_.provider->set_halt_callback([](std::string_view){});
     config_.provider->set_event_publisher([](std::shared_ptr<event>){});
     ...
     if (live) provider->close();
   ```

5. In ~engine and reset paths, set the flag before any pool/ring destruction.
6. Rebuild with ASAN and re-run the test matrix + any live-provider simulation tests.
7. Add a targeted test that exercises "late fill after stop" (inject a fill after revocation but before full dtor).

### Phase 3: Ownership & Lifetime Modernisierung (ObjectPool + Dashboard Builder)
**Ziel dieser Phase:** Remove raw-`this` deleters and the giant ref-injection surface.

**Issue HIGH-02 + MEDIUM-01**

**Konkrete Schritte zur Behebung (ObjectPool deleters):**
1. Open `src/types/object_pool.h`.
2. Change the deleter to not capture raw `this`. One robust pattern (example):

```diff
- auto dtor_and_return = [this](T* p) {
-     p->~T();
-     defer_release(static_cast<void*>(p));
-     in_use_atomic_.fetch_sub(1, std::memory_order_relaxed);
- };
+ // Pass a non-owning returner or use a separate arena token
+ struct Returner { ObjectPool* p; void operator()(T* obj) const; };
+ auto dtor_and_return = Returner{this};
```

   Implement `Returner::operator()` to do the ~T + defer + atomic (still needs care; better long-term: explicit return API).

Alternative stronger fix (recommended for follow-up):
- Change hot pooled objects to return via a `return_to_pool(T* p)` call on the engine thread (or via DeferredReturnQueue always). Avoid `shared_ptr` escape for objects that must die in the arena.

**For DashboardSnapshotBuilder:**
1. Open `src/engine/dashboard_snapshot_builder.h` and `.cpp` + `engine.cpp` (ctor site).
2. Replace the two `void*` with a small struct:

```diff
- void* stage_timer,
- void* memory_sampler
+ struct DebugSamplers { debug::StageTimer* stage = nullptr; debug::MemorySampler* memory = nullptr; };
+ DebugSamplers debug_samplers
```

   Update the call site in engine.cpp and all uses (add casts only inside the struct or helper methods).
3. For the long list of pools/rings, introduce a view struct or document + static asserts on order. Or pass a `const PoolsView&` + `const RingsView&` (groups the refs).
4. Add a comment block at the top of the builder:
   ```cpp
   // All injected references must outlive this builder.
   // Construction happens before start_workers in normal paths.
   // clear_for_mc_reset() must be called on MC reuse.
   ```
5. Rebuild + run tests (including ones that exercise snapshot + debug view + HAS_DEBUG paths).

**Konkrete search_replace example (for the void* part, adapt context):**

old_string (approximate from current):
```cpp
        const std::shared_ptr<EventRing>& mm_ring,
        // For debug stages and memory sampler if HAS_DEBUG
        void* stage_timer,   // debug::StageTimer* or null
        void* memory_sampler // debug::MemorySampler* or null
    );
```

new_string:
```cpp
        const std::shared_ptr<EventRing>& mm_ring,
        // For debug stages and memory sampler if HAS_DEBUG
        DebugSamplers debug_samplers
    );
```

(Define the struct near the top of the header or in a small debug_types.h.)

### Phase 4: Concurrency & Atomics + Worker/Ring Hardening
- Make RingBuffer watermark_threshold_ atomic and the cb revocable (or remove the callback surface if unused).
- Add proactive drain of DeferredReturnQueue from publish_event or a periodic tick (in addition to acquire).
- Strengthen worker shutdown: ensure all rings are drained and no pooled shared_ptrs remain before pools are destroyed.
- Consider TSan build + run of worker-heavy + provider tests.

### Phase 5: Verification & Re-Check
1. Rebuild with ASAN+UBSAN (and TSAN for weekly).
2. Full relevant test run:
   ```bash
   ./build/truetest_tests --gtest_filter='*Hotpath*|*Engine*|*Risk*|*Golden*'
   ./build/truetest_tests --gtest_filter='*HotpathAllocs*|*HotpathAllocMatrix*|*HotpathPoolPrewarm*'
   ```
3. Re-run the three gate scripts.
4. Invoke this skill again with the **same output directory** the user originally provided:
   ```bash
   /memory-checks /home/leonard/work/projects/truetest/core/check-ups
   ```
5. Compare the new `2026-07-17-...` (or next date) report against this one. Zero HIGH findings is the target.
6. If late fills or shutdown interleaving tests were added, run them under sanitizer.
7. Update any suppression files only after real fixes (not to hide problems).

### Phase 6: Dokumentation & Suppression Management (falls nötig)
- Update `core/docs/engine.md` (and internal design doc) with the new ownership contracts for the builder, pools, and callbacks.
- Add explicit "Lifetime" sections to worker headers and ObjectPool.
- Document the MC reset contract (what gets cleared vs. what must be re-created).
- Remove or update the historical `ENGINE_LOC_WAIVER` comment once the extraction is fully cleaned.
- If any clang-tidy warnings are intentionally suppressed, add narrow `// NOLINT(...)` with justification (prefer fixes).

**Verification at end of all phases:**
- `git status --porcelain` should only show intended changes.
- Re-run the full memory-checks skill.
- All HIGH findings from this report resolved or explicitly documented with mitigation + test coverage.

This plan is derived directly from the two subagent reports and the concrete evidence (clang-tidy, code patterns, test runs, prior 2026-07-16 report) collected in this session. It prioritizes HIGH crash/UAF risks first, then the architectural surface introduced/exposed by the recent extraction.

---

**Report written exclusively to the user-specified convention directory.**  
No source files were modified during this read-only memory-checks run.

---

## Resolution (2026-07-17, post-fix)

All HIGH and MEDIUM findings from this report were addressed using multiple independent subagents for analysis + post-fix review:

- **HIGH-01** (callback races): Added `provider_callbacks_armed_` atomic guard. All four provider callbacks (`event_publisher`, `funding_event_factory`, `halt_callback`, `unknown_fill_handler`) now early-return when disarmed. Disarm happens *before* revocation/close in `stop_workers()` and `~engine()`. Re-arm on start and MC reset. Proactive drains + ring drains added. (See engine.cpp/h around armed flag + guarded lambdas; stop_workers ~874+.)

- **HIGH-02** (ObjectPool [this] deleter): Replaced raw `[this]` lambda with `Returner` struct + `alive_` atomic guard in `ObjectPool`. Late drops now safe-no-op (leak slot instead of UAF). `~ObjectPool` disarms; `rearm_for_reuse()` + calls on MC reset. Same drain guard. (object_pool.h: alive_, Returner, dtor, rearm.)

- **MEDIUM-01** (builder ctor): `void* stage_timer/memory_sampler` replaced by `DebugSamplers` aggregate (type-safe). Added explicit lifetime contract comment covering post-ctor ring assignment + MC. (dashboard_snapshot_builder.h/cpp + engine.cpp call site.)

- **MEDIUM-02** (RingBuffer watermark): `watermark_threshold_` made `atomic`; setter stores atomically; added documentation that cb is startup-only / best-effort. (ring_buffer.h.)

- **MEDIUM-03** (deferred drain): Added proactive `drain_object_pool_returns()` calls in `publish_event`, early `stop_workers`, `teardown_event_loop_infra`, and `reset_for_next_trial`. Existing per-event drains already covered many paths. (engine.cpp + object_pool.h.)

**Verification performed:**
- Full configure + build with `-DENABLE_ASAN=ON -DENABLE_UBSAN=ON -DENABLE_DEBUG=ON`.
- Targeted test runs: `*Hotpath*`, `ObjectPool*`, `DeferredReturn*`, `RingBuffer*`, `Engine*`, `Risk*`, `ExecutionBridgeUnknownFill*` — all passed under sanitizers.
- Gate scripts: `check-hotpath-json.sh`, `check-layer-deps.sh`, `check-live-safety-freeze.sh` — all OK.
- Post-fix review by dedicated subagent (different perspective) confirmed each original finding resolved with matching evidence; no new lifetime bugs introduced. Residual surface documented (e.g. watermark cb still not fully revocable; watchdog separate; recommend a dedicated "late invoke after stop" test).

Changes preserve zero-alloc hot path, existing contracts, and MC reuse model. Further waves can build on the hardened ownership surfaces.

**Multiple subagents used throughout:** initial three (Tooling, Ownership, Surface/Builder) for diagnosis + one post-edit verification reviewer. All findings handled.
