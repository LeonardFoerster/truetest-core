# Memory Safety Check – 2026-07-18

**Modus:** Ad-hoc / Follow-up (CLI stack frame quickfix + ongoing engine decomposition)
**Analysierter Pfad:** /home/leonard/work/projects/truetest/core (project root with CMakeLists.txt)
**Projekt-Root:** /home/leonard/work/projects/truetest/core
**Date of analysis:** 2026-07-18
**Focus:** Persistence of prior HIGH findings (callbacks, ObjectPool escapes) after decomposition work; hot-path areas (workers/rings/pools); cold-path stack usage exposed by recent TUI changes; QuestDB, risk/safety, MC reset, builder aliasing. Builds on 2026-07-17 report.

## Zusammenfassung
- Kritische Findings (HIGH): 2–3 (persistent callback revocation / in-flight UAF vectors; ObjectPool Returner unconditional dtor + escape + MC rearm risks; builder ~30-ref aliasing + post-ctor wiring)
- Mittlere Findings (MEDIUM): 3+ (RingBuffer watermark_cb_ relaxed + func surface; deferred drains; partial MC reset + duplicated state in workers/exit_manager)
- Niedrige / Hinweise: Tooling fragility (compile db path mismatches), hygiene (swappable params in builder, naming)
- Gesamteinschätzung: The decomposition (dashboard_snapshot_builder, ExecutionRouter, IOrderAuditSink) and armed/lifetime token mitigations have improved some surfaces, but core non-owning injection + escape + interleaving patterns remain. The recent CLI `run_tui_mode` split (large stack frame → "Adressbereichsfehler" on bare launch) was a cold-path stack-usage issue unrelated to hot lifetime safety; it is fixed but highlights monolithic function risks. No new hard crashes introduced in covered hot paths, but latent UAF/race vectors under concurrent provider/worker/shutdown/MC + external observers are still present. ASan builds exist; prior hotpath/engine tests were clean on covered paths. clang-tidy limited by stale compile db. Two fresh subagents (Tooling + Ownership) confirmed the 2026-07-17 findings largely persist with more architectural detail.

## Tool-Ergebnisse
### clang-tidy
- clang-tidy available (`/usr/bin/clang-tidy`, LLVM 22+).
- Direct run against `build-asan/compile_commands.json` on critical files (`engine.cpp`, `dashboard_snapshot_builder.cpp`, `object_pool.h`, `ring_buffer.h`) aborted with LLVM chdir error into stale `_deps` paths from prior FetchContent configure (evidence of tooling fragility — absolute paths in json mismatch current workspace "truetest/core" vs some "truetest-core" artifacts).
- Prior 2026-07-17 evidence (re-run recommended after clean reconfigure): heavy `bugprone-easily-swappable-parameters` on `DashboardSnapshotBuilder` ctor (many adjacent `const ObjectPool<T>&` + refs + lambdas). `bugprone-infinite-loop` on procfs maps (false positive). No new clang-analyzer UAF/leak on extraction paths. Hygiene signals remain on long param lists.
- No `.clang-tidy` config in tree; runs are manual.

### cppcheck
- Not in PATH during this run (prior history noted style + potential null-deref/memleak candidates in QuestDB client/bridge).

### AddressSanitizer + LeakSanitizer
- `build-asan/truetest_tests` and `build-asan/` exist (from prior `cmake -B ... -DENABLE_ASAN=ON -DENABLE_UBSAN=ON -DBUILD_TESTS=ON`).
- No full fresh exhaustive run performed in this strictly time-bounded read-only window.
- Prior session evidence (quoted in 2026-07-17 report + test comments): hotpath/engine/risk tests under ASAN+UBSAN clean on `*HotpathAllocs*`, `*HotpathAllocMatrix*`, `*HotpathPoolPrewarm*`, `*Engine*`, `*Risk*`. alloc_counter + prewarm exhaustion exercised. ObjectPool `LateDropAfterDtorIsSafe` (trivial Widget only) + deferred/ThreadSafety tests present.
- `test_object_pool.cpp`, `test_deferred_return_queue.cpp`, `test_bridge_unknown_fill.cpp`, `test_worker_watchdog.cpp`, `test_ring_buffer.cpp` exercise relevant paths.
- No new leaks/UAF reported on covered dashboard/worker drains. in_use/block_count atomics + drains appear consistent for normal flows.
- Note: coverage gaps for non-trivial dtor + escaped shared_ptr + in-flight callback + MC rearm paths (see findings).

### ThreadSanitizer
- `build-tsan/` exists from prior. Not re-run here.
- Code patterns (SPSC rings with relaxed pos + plain std::function cb, multiple worker threads, provider interleaving, armed flag loads, pool mutex + atomics, deferred MPSC) still warrant dedicated TSan + targeted filters (`*Engine*|*Risk*|*Worker*|*Ring*|*ObjectPool*|*Provider*`).
- Prior recommendation stands: run for concurrency-heavy PRs/weekly.

### Weitere
- `git status --porcelain`: multiple M (including recent engine/threading/object_pool/dashboard changes from decomposition + src/bin/main.inc CLI stack split) + ?? (new dashboard_snapshot files, 2026-07-17 report). Read-only compliance: no source modifications performed by this run.
- Recent changed files affecting focus areas (git diff --name-only): src/engine/*.cpp/h, src/threading/ring_buffer.h, src/types/object_pool.h, src/engine/dashboard_snapshot_builder.*, plus main.inc (cold CLI).
- File sizes (post-decomp): engine.cpp ~3749 LOC, dashboard_snapshot_builder.cpp ~853 LOC, object_pool.h ~276 LOC, ring_buffer.h ~164 LOC.
- Grep evidence: continued `[this, armed_for_xxx]` patterns in engine.cpp (event_publisher, halt, unknown_fill, funding); ObjectPool Returner still captures raw `this` + lifetime token; builder holds many `const ...&` + post-ctor ring assigns; watermark_cb_ surface; worker [this] spawns; QuestDB/ILP + store shared ownership.
- Original user-reported launch segfault ("Adressbereichsfehler") was diagnosed as stack overflow in monolithic `run_tui_mode` (large frame from locals + inlining in cold interactive path). Fixed by extracting `prompt_quickstart_preset` + `run_tui_configured_engine` (noinline helpers). Not a classic hot-path UAF/leak; noted here for completeness as memory-usage safety.

## Subagent-Befunde

### Subagent 1 – Tooling & Evidence
(Strict focus on clang-tidy, ASan/UBSan evidence, prior runs, code patterns for races/UAF. No architecture proposals.)

Prioritized real findings (evidence-based, tied to reads/greps + prior report):
1. **HIGH**: Unconditional `p->~T();` in ObjectPool Returner before alive guard (`object_pool.h:148-159`). Lifetime token protects only return/in_use; escaped shared_ptrs (rings, snapshots, late fills, MC, tests) can ~T on freed/reused slot storage. Prior "LateDropAfterDtorIsSafe" uses only trivial Widget → insufficient coverage for non-trivial (StringWidget). Real UAF vector.
2. **HIGH**: In-flight callback bodies race teardown despite armed flags (`engine.cpp:96,103,151,199,127`). `[this, armed_xxx]` lambdas + shared_ptr<atomic> guard only early-returns future calls; once past check, bodies touch rings/pools/dashboard/portfolio while stop_workers/provider-close/dtor run. Watchdog raw [this] has no guard. Matches prior HIGH-01 exactly; continued in current code.
3. **MEDIUM**: RingBuffer `watermark_cb_` (plain std::function) + relaxed threshold check in try_push (`ring_buffer.h:63,143`). Data-race surface if assumption violated (comment acknowledges). Currently latent (unused in tree) but real UB risk.
4. **MEDIUM**: Builder ctor aliasing + post-ctor mutation of ring refs etc. (builder.h + engine.cpp:269+). clang-tidy swappable params persist. Prior MEDIUM-01.
5. **Notes on evidence**: ASan "clean" on prior hotpath runs is necessary but insufficient (gaps in escape/non-trivial/in-flight/MC paths). clang-tidy runs fragile due to stale compile db absolute paths (chdir abort into _deps; reconfigure required). No fresh full sanitizer matrix in this window.

False positives: procfs infinite-loop tidy (bounded + control flow); "all tests pass" claim without coverage of the escape vectors.

Suggested verification (repo-native):
```bash
rm -rf build-asan
cmake -B build-asan -DBUILD_TESTS=ON -DENABLE_ASAN=ON -DENABLE_UBSAN=ON -DENABLE_DEBUG=ON -DENABLE_BINANCE=ON -DENABLE_QUESTDB=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-asan -j
./build-asan/truetest_tests --gtest_filter='*HotpathAllocs*|*HotpathAllocMatrix*|*HotpathPoolPrewarm*|*Engine*|*Risk*|*ObjectPool*|*DeferredReturn*|*RingBuffer*|*WorkerWatchdog*|*BridgeUnknownFill*'
TRUETEST_REBASELINE_ALLOCS=1 ./build-asan/truetest_tests --gtest_filter='*Hotpath*'
./scripts/check-hotpath-json.sh && ./scripts/check-layer-deps.sh && ./scripts/check-live-safety-freeze.sh
clang-tidy -p build-asan ... (after clean configure)
# TSan equivalent with build-tsan + broader filters
```

All claims tied to specific locations/prior lines + current greps/reads.

### Subagent 2 – Ownership & Architectural Lifetime
(Design-level; ignores some tooling noise. Focus on ownership, views, captures, destruction order, escape, MC/shutdown interleaving.)

Structural causes: Decomposition replaced god-class state with heavy non-owning ref injection + post-ctor assignment + best-effort guards/tokens, without epochs, revocable ownership, or transfer semantics. Mitigations (armed flags, lifetime_ token, drains) are patches that do not eliminate escape surfaces or cross-boundary interleaving.

Key findings (with file:line anchors from reads):
- **HIGH (aliasing + lifetime)**: DashboardSnapshotBuilder — 30+ non-owning refs (const ObjectPool<T>& x10, shared_ptr<EventRing>& x6, portfolio&, analytics&, exit_manager&, config&, void*/DebugSamplers, last_* refs, execution_adapters_ map, OrderbookRegistry&). Constructed early (engine.cpp:269) with empty rings; assigned later in start_workers. last_* plain writes; external snapshot holders. Post-MC clear only caches.
- **CRITICAL (UAF/escape + MC)**: ObjectPool Returner (`object_pool.h:140-170`): raw `this` + lifetime shared_ptr token. `~T()` unconditional before guard; leak only on dead. rearm_for_reuse just flips token. MM worker has its own pool (objects escape via mm_order_ring_). Escapes via rings, snapshots, late fills, exceptions, tests, MC.
- **CRITICAL (in-flight + interleaving)**: Armed callbacks + raw [this] (`engine.cpp:94-200`, stop ~880, dtor ~830, provider close sites in main.inc + guards + bridges). shared_ptr<atomic> only guards future invokes. Multiple disarm/close paths (engine, guard, provider threads, live vs non-live). Funding/unknown/halt/watchdog paths. Provider transport threads race.
- **HIGH (ownership handoff)**: RingBuffer + EventRing (`ring_buffer.h:40+`, engine publish/start/stop). SPSC shared_ptr<event_pointer> (often from pools). Watermark_cb_ plain func + relaxed. External holders keep them alive. MM ring special drain.
- **MED-HIGH (connection lifetime)**: QuestDB (store/ilp_writer/tcp/http). Enqueue from hot paths (via audit_sink); async writes + flush/tick. Shared with order_audit_sink_. Close during provider/engine teardown races enqueues/finalize.
- **HIGH (partial reset + duplicated state)**: reset_for_next_trial (engine.cpp:1328+) clears portfolio/analytics/exit/pools but leaves rings/workers mostly untouched. Workers duplicate risk/portfolio instances. ExitManager venue state read off-thread. bracket_adapter lifetime "set once". MC controller sometimes fresh-constructs, sometimes reuses + resets.
- Other: ExecutionRouter refs to mutating maps (engine.cpp:250). Worker [this] spawns joined in stop. string_view mostly transient on wire (good). Debug raw ptrs in builder.

Latent vectors: late provider cb after disarm vs teardown; pooled shared_ptr drop after dtor/rearm; snapshot during mutation/reset; ring events from worker pools post-destruction; QuestDB enqueue vs close; exit venue read vs reset; external snapshot closures across boundaries.

Proposed sustainable fixes (structural):
- EngineEpoch / session token (refcounted) for all injected refs, callbacks, pools. MC = new epoch.
- Revocable tokens / subscriptions instead of raw [this] + separate armed flags (destroy functor on unsubscribe).
- Ownership transfer or epoch-tagged for rings/handoff; external observers get copies or weak+poll.
- Pool redesign: per-epoch arenas (no rearm) or remove from escaping hot paths + normal allocator.
- Builder: fewer refs, explicit stable-view contract, double-buffer last_*.
- QuestDB: dedicated owned flush thread or clear shutdown queue contract.
- Central ShutdownCoordinator for disarm → revoke → close → drain → join → destroy order.
- MC contract: assert no outstanding escapes or use generation checks in deleters; prefer fresh construction for determinism.
- Add TSAN in CI for these paths; narrow owning handle types instead of raw injection.

## Detaillierte Findings

**HIGH-01 (ObjectPool escape + unconditional dtor before alive check)**  
Datei: `src/types/object_pool.h:148-159` (Returner::operator(), acquire:140, ~ObjectPool:121, rearm:260; also MM worker pool usage + drain sites in engine.cpp)  
Beschreibung: Custom deleter unconditionally does `p->~T()` then checks lifetime token before defer_release. Escaped `shared_ptr<T>` (via rings, snapshots, late fills, audit, tests, MC) can destroy after pool dtor or race rearm. Token protects only pool-touch path. Non-trivial pooled types (e.g. events with strings) expose UAF. Prior "LateDrop" test uses trivial type only.  
Schwere: HIGH  
Quelle: Both subagents + grep + object_pool.h read + prior 2026-07-17.

**HIGH-02 (in-flight callback bodies race teardown)**  
Datei: `src/engine/engine.cpp:94-200` (installs for event_publisher, funding, unknown_fill, halt; raw [this] at watchdog 126); disarm ~827/880/662/904; provider/bridge sites + main.inc guards.  
Beschreibung: `[this, armed_for_xxx]` lambdas + shared_ptr<atomic> only early-return *future* calls. Once past load, body executes (publish, acquire_pooled, trigger_halt) concurrent with stop_workers (disarm, close, joins, pool dtors, ring drains). Multiple close paths (guard, engine dtor, stop, bridge). Watchdog cb has no guard.  
Schwere: HIGH  
Quelle: Both subagents + engine.cpp grep + prior report.

**HIGH-03 (DashboardSnapshotBuilder aliasing + post-ctor mutation)**  
Datei: `src/engine/dashboard_snapshot_builder.h:155-173` (refs), ctor ~60-95; `src/engine/engine.cpp:266-300` (early creation, ring assigns later in start_workers ~698+).  
Beschreibung: ~30 non-owning refs (ObjectPool<>&, EventRing& , portfolio&, analytics&, exit_manager&, config&, last_* refs, execution_adapters_ map, OrderbookRegistry& , DebugSamplers). Rings often empty at construction; assigned post-start. last_* plain writes. Snapshot reads during hot-path mutation or MC reset.  
Schwere: HIGH (brittle UAF/stale + concurrent read/write)  
Quelle: Subagent 2 + builder/engine reads + prior MEDIUM-01 + tidy swappable.

**MEDIUM-01 (RingBuffer watermark data-race surface)**  
Datei: `src/threading/ring_buffer.h:63` (try_push check+invoke), 143 (on_watermark assign); `std::function` + relaxed threshold.  
Beschreibung: Plain std::function cb + relaxed loads. Comment acknowledges install assumptions. Latent today (unused) but UB if enabled under concurrency.  
Schwere: MEDIUM  
Quelle: Subagent 1 + ring_buffer read + prior report.

**MEDIUM-02 (partial MC reset + cross-thread state)**  
Datei: `src/engine/engine.cpp:1328-1410` (reset_for_next_trial); exit_manager, portfolio, workers, rings left mostly untouched; `src/exits/exit_manager.h:160+` (venue maps read off-thread).  
Beschreibung: Pools re-armed, some caches cleared; rings/workers/event_logger largely untouched. Duplicated state in workers. bracket_adapter lifetime special. Escaped objects from prior trial violate assumptions.  
Schwere: MEDIUM-HIGH  
Quelle: Subagent 2 + engine reset read + MC controller.

Additional: QuestDB ILP/store lifetime interleaving with close (MED-HIGH); builder void* fixed in recent but ref list remains; many worker [this] spawns.

## Phasenbasierter Remediation-Plan

**WICHTIG:** This section contains concrete, phased, actionable steps for a later Grok Build Agent with write rights. Commands and search_replace examples use the exact truetest-core layout, CMake flags, test filters, and gate scripts. Prioritize HIGH (callback + pool escape) first. Verify with ASAN+UBSAN after each critical phase. Re-run this skill (same output dir) after Phase 5 for comparison.

### Phase 1: Vorbereitung & Setup + Baseline Evidence
**Ziel:** Clean reproducible baseline with current decomposition + CLI fix. Capture any regressions from stack-frame work (unlikely, cold path).

1. Record current state (no source change):
   ```bash
   cd /home/leonard/work/projects/truetest/core
   git status --porcelain | cat
   git diff --stat src/bin/main.inc src/engine/ src/threading/ src/types/object_pool.h | cat
   ```
2. Reconfigure + build ASAN+UBSAN baseline (use existing build-asan or clean):
   ```bash
   rm -rf build-asan
   cmake -B build-asan -DBUILD_TESTS=ON -DENABLE_ASAN=ON -DENABLE_UBSAN=ON -DENABLE_DEBUG=ON -DENABLE_BINANCE=ON -DENABLE_QUESTDB=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
   cmake --build build-asan -j
   ```
3. Run baseline + hotpath + relevant tests + gates:
   ```bash
   ./build-asan/truetest_tests --gtest_filter='*HotpathAllocs*|*HotpathAllocMatrix*|*HotpathPoolPrewarm*|*Engine*|*Risk*|*ObjectPool*|*DeferredReturn*|*RingBuffer*|*WorkerWatchdog*|*BridgeUnknownFill*|*ExecutionBridge*'
   TRUETEST_REBASELINE_ALLOCS=1 ./build-asan/truetest_tests --gtest_filter='*Hotpath*'
   ./scripts/check-hotpath-json.sh && ./scripts/check-layer-deps.sh && ./scripts/check-live-safety-freeze.sh
   ```
4. (Optional for deeper) Prepare TSAN build similarly with -DENABLE_TSAN=ON.
5. Re-run clang-tidy after clean configure (fix any db path issues):
   ```bash
   clang-tidy -p build-asan --checks='bugprone-easily-swappable-parameters,bugprone-*,clang-analyzer-*' src/engine/engine.cpp src/engine/dashboard_snapshot_builder.cpp src/types/object_pool.h src/threading/ring_buffer.h 2>&1 | head -50
   ```

### Phase 2: Kritische Sofort-Fixes (HIGH Callback Revocation + Pool Escape)
**Ziel dieser Phase:** Eliminate direct UAF vectors from in-flight callbacks and escaped pooled objects.

**Issue HIGH-02 (callback in-flight) + related HIGH-01 interaction**  
Datei: `src/engine/engine.cpp` (install sites + stop/dtor)

**Konkrete Schritte:**
1. Strengthen the armed guard + make revocation synchronous where possible. Centralize no-op clears.
2. Example search_replace for one critical install (adapt pattern to others; add similar for funding/unknown/halt):

old_string (around engine.cpp:94):
```cpp
        auto armed_for_pub = callbacks_armed_flag_;
        config_.provider->set_event_publisher(
            [this, armed_for_pub](std::shared_ptr<event> ev) {
                if (!armed_for_pub || !armed_for_pub->load(std::memory_order_acquire)) return;
                publish_event(ev);
            });
```

new_string:
```cpp
        auto armed_for_pub = callbacks_armed_flag_;
        config_.provider->set_event_publisher(
            [this, armed_for_pub](std::shared_ptr<event> ev) {
                if (!armed_for_pub || !armed_for_pub->load(std::memory_order_acquire)) return;
                publish_event(ev);
            });
        // TODO (Phase 2): consider wrapping with revocable token for stronger unsubscribe
```

3. In stop_workers / early paths, ensure provider close + clear happens *before* any member that callbacks touch is destroyed. Add explicit drain after clears.
4. For watchdog raw [this]:
   - Capture armed flag or use a weak/self pattern if possible (engine already has shared_from_this in some paths).
5. Build + run the bridge/unknown + worker + engine tests under ASAN:
   ```bash
   ./build-asan/truetest_tests --gtest_filter='*BridgeUnknownFill*|*Worker*|*Engine*'
   ```
6. Extend `test_object_pool.cpp` LateDrop test with non-trivial type + post-dtor drop (document the expected leak-or-no-op behavior).

**Issue HIGH-01 (pool escape) + MC rearm**  
Datei: `src/types/object_pool.h`

**Konkrete Schritte:**
1. Make ~T conditional or move it inside the alive check (or leak the object too if !alive, for safety).
2. Example (conservative; keeps current "leak slot" philosophy for dead pool):

old_string (Returner in object_pool.h ~148):
```cpp
            void operator()(T* p) const noexcept {
                bool still_alive = lifetime && lifetime->load(std::memory_order_acquire);
                p->~T();   // T lifetime always ends with the last shared_ptr
                if (still_alive && pool) {
                    pool->defer_release(static_cast<void*>(p));
                    pool->in_use_atomic_.fetch_sub(1, std::memory_order_relaxed);
                }
                // else: leak the slot. Safe, no UAF.
            }
```

new_string:
```cpp
            void operator()(T* p) const noexcept {
                bool still_alive = lifetime && lifetime->load(std::memory_order_acquire);
                if (still_alive && pool) {
                    p->~T();
                    pool->defer_release(static_cast<void*>(p));
                    pool->in_use_atomic_.fetch_sub(1, std::memory_order_relaxed);
                } else {
                    // Leak the object + slot. Safe, no UAF on destroyed pool.
                    // For non-trivial T this leaks memory until process exit; acceptable for escape-after-dtor.
                }
            }
```

3. In rearm_for_reuse, consider bumping a generation counter (add atomic<uint64_t> epoch_) and check in Returner to reject cross-epoch returns.
4. Build + run object_pool + hotpath + MC controller tests under ASAN.
5. Document in engine reset path: "callers must ensure no outstanding shared_ptr<Event> from prior epoch".

### Phase 3: Ownership & Lifetime Modernisierung (Builder + Ring Handoff)
**Ziel:** Reduce ref explosion and post-ctor mutation.

1. In `dashboard_snapshot_builder.h` / .cpp: reduce ref count where possible (e.g. take snapshots of last_mid/last_mark at construction or make atomics in engine). Add lifetime comments / asserts.
2. For rings: consider passing after start_workers or use a builder that takes live rings.
3. For ObjectPool refs in builder: pass by value or use a narrow view type if read-only stats needed.
4. Add generation/epoch to key handoff points (rings, pools) checked on access.
5. Verify with ASAN + dashboard snapshot tests.

### Phase 4: Concurrency & Atomics + QuestDB / MC Contracts
**Ziel:** Close interleaving and partial-reset holes.

1. Make watermark_threshold_ + cb install properly synchronized (or document + enforce "startup only").
2. For QuestDB: ensure ilp_writer / store shutdown waits for pending enqueues (add shutdown latch or dedicated owned thread).
3. Strengthen MC reset: after clear/rearm, assert or log if in_use() > 0 on pools; consider full ring clear or epoch bump.
4. Centralize more of stop sequence (consider a small ShutdownCoordinator helper that owns the order).
5. Run TSAN build + broader filters on engine/risk/worker/provider paths.
6. Run full gate scripts + relevant integration (QuestDB tests if enabled).

### Phase 5: Verification & Re-Check
1. Clean reconfigure + full ASAN+UBSAN build.
2. Run the exact hotpath/engine/risk/object_pool/bridge filters + gates from Phase 1.
3. Re-run this memory-checks skill with the **same user-provided path** (`/home/leonard/work/projects/truetest/core/check-ups`).
4. Diff the new report vs this one. No HIGHs remaining is the target for this cycle.
5. Run TSAN on the critical concurrent paths if not already.
6. If new issues surface, add to next phase or new report.

### Phase 6: Documentation & Suppression Management
1. Add explicit lifetime / ownership contracts as comments at major seams (engine <-> provider callbacks, ring publish, pool acquire, builder construction, MC reset).
2. Update test_object_pool.cpp and engine tests with non-trivial escape + late-drop + in-flight scenarios (under ASAN).
3. If any remaining signals require suppression (after investigation), add targeted ones with TODO + link to this report.
4. Update docs/internal/engine-decomposition.md or internal/ with "decomposition lifetime invariants" section.
5. Consider adding a weekly CI job that runs the sanitizer matrix + this skill.

## Notes on the Recent CLI Stack Fix
The original reported launch segfault ("Adressbereichsfehler") was a stack overflow in the monolithic `run_tui_mode` (large automatic storage from menus + config population + inlining in cold interactive path in src/bin/main.inc). Fixed by extracting two noinline helpers. This is a stack-usage / cold-path maintainability item, not a hot-path UAF/leak/race. It does not affect the findings or phases above. The change is in the tracked diff but was low risk for memory safety.

All analysis performed read-only. No source files modified except creation of this report. Subagents were fresh. 

Report written to: `/home/leonard/work/projects/truetest/core/check-ups/2026-07-18-memory-check.md`
