# Memory Safety Check – 2026-07-16

**Modus:** PR / Feature (large set of changes touching live-safety surface + new async support)
**Analysierter Pfad:** check-ups/
**Projekt-Root:** /home/leonard/work/projects/truetest-core (detected via CMakeLists.txt, src/, tests/)

## Zusammenfassung

- Kritische Findings (HIGH): 3 (cross-thread callback lifetime on new async unknown_fill_handler; ObjectPool custom deleter lifetime; provider/bridge ownership vs engine `this` captures + missing clear on shutdown)
- Mittlere Findings (MEDIUM): 4 (worker thread spawn/join ordering, ring watermark callbacks, multiple transport [this] captures in binance paths, QuestDB connection + string_view handling)
- Niedrige / Hinweise: mehrere (clang-tidy style/cppcoreguidelines on pools/rings, third-party registry dtor mismatches in valgrind)
- Gesamteinschätzung: Hot path (allocs, pools, rings of shared_ptr<event>) currently stable under normal operation and passes its allocation budgets. Structural lifetime and shutdown ordering risks are concentrated in cold paths (ctor wiring of callbacks, live halt/shutdown, MC reuse, provider close interleaving with engine destruction). The newly introduced `IAsyncSubmitSupport` + `unknown_fill_handler` is the clearest recent exemplar of pre-existing patterns. Full dynamic sanitizer runs (ASAN/UBSAN/TSAN) were launched but did not complete in the available window; evidence is therefore a combination of static + test + subagent analysis. No heap leaks detected by Valgrind on exercised basic paths (0 bytes in use at exit).

## Tool-Ergebnisse

### clang-tidy
Targeted runs on `src/threading/ring_buffer.h`, `src/types/object_pool.h`, `src/execution/async_support.h` (and attempted on engine/bridge) using `build-asan/compile_commands.json`:
- Primarily style and cppcoreguidelines (naming of structs/vars, magic numbers 4096/64, special-member-function rules, `reinterpret_cast` + pointer arithmetic in pool placement, `cppcoreguidelines-owning-memory` on placement new).
- No direct "use after free", "dangling pointer", or leak diagnostics surfaced for the analyzed core files.
- Many warnings suppressed (third-party headers).
- Full run on large files (engine.cpp) was slow.

### cppcheck
Not available in the environment (not installed). Skipped.

### AddressSanitizer + LeakSanitizer / UBSan
- Build configured: `cmake -B build-asan -DBUILD_TESTS=ON -DENABLE_ASAN=ON -DENABLE_UBSAN=ON -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON .`
- Target build launched (`cmake --build build-asan --target truetest_tests`). Did not complete within analysis window (killed after evidence collection).
- Compiler flags correctly wired in `cmake/CompilerFlags.cmake` (mutual exclusion of TSAN vs ASAN+UBSAN, applied via `tt_apply_common_flags` to compile + link).
- No ASAN/UBSAN crash or error logs available from this run.

### ThreadSanitizer
Not executed (separate build required; mutually exclusive per CMake).

### Valgrind
Run on basic engine test (`Engine.BasicConstruction` filter) using non-ASAN build:
- `All heap blocks were freed -- no leaks are possible` (0 bytes in use at exit).
- Many "Mismatched free() / delete / delete []" reported in gtest/StrategyRegistry/ProviderRegistry/boost::asio ssl static/global destructors at process exit. These are classic artifacts from heavy static initialization + custom registries + third-party libs; not indicative of engine hot-path or live component leaks.
- No engine-specific UAF or leaks in the short path exercised.

### Hot-path allocation evidence (alloc_counter + pools)
- `./build/truetest_tests --gtest_filter='HotpathAllocs*'`
  - `HotpathAllocs.SmaGolden_30Bars_PostWarmupUpperBound` — PASS (within Phase-4 bound ≤2200 allocs / ≤6.5M bytes; 0 pool grows).
  - `HotpathAllocs.SmaSynthetic_1000Bars_PostWarmupUpperBound` — PASS (within bound; 0 pool grows).
- Rebaseline mechanism documented: `TRUETEST_REBASELINE_ALLOCS=1 ./build/truetest_tests --gtest_filter='HotpathAllocs.*'`
- Other hotpath/pool tests (`test_hotpath_alloc_matrix.cpp`, `test_hotpath_pool_prewarm.cpp`, `test_object_pool.cpp`, `test_control_block_pool.cpp`, `test_deferred_return_queue.cpp`) exist and are part of the zero-alloc discipline.

### Project gate scripts
- `scripts/check-live-safety-freeze.sh` — OK (surface respected).
- `scripts/check-hotpath-json.sh` — OK (nlohmann/json confined).
- `scripts/check-layer-deps.sh` — OK.

## Subagent-Befunde

### Subagent 1 – Tooling & Evidence
(Full output summarized and incorporated; see detailed HIGH/MEDIUM items below. The subagent performed its own greps, file reads, and cross-checks against the provided evidence. Key conclusion: dynamic sanitizer coverage is the missing piece; existing tests + alloc counters + static style checks do not substitute for ASAN/TSAN runs on shutdown + bracket-fill interleavings.)

**Prioritized evidence-based findings from tooling subagent** (HIGH):
1. `unknown_fill_handler` installed with `[this]` and never cleared; invoked on fill-transport worker while engine state (`exit_manager_`) can be destroyed.
2. ObjectPool custom deleter `[this]` + placement-new objects returned as `shared_ptr` that can escape via rings/workers.
3. ExecutionBridge on_message / transport_loop registrations vs. provider-owned lifetime and engine destruction.

Verification commands recommended by the subagent (exact repo lines):
```bash
cmake -B build-asan ... -DENABLE_ASAN=ON -DENABLE_UBSAN=ON ...
ASAN_OPTIONS=... UBSAN_OPTIONS=... ./build-asan/truetest_tests --gtest_filter='*HotpathAllocs*|*Engine*:*Bridge*:*UnknownFill*:*AsyncSupport*'
# plus TSAN build + focused Valgrind + the three check-*.sh
```

### Subagent 2 – Ownership & Architectural Lifetime
(Complete structured report received. Major systemic observation: engine-centric state + provider-owned execution plumbing + ad-hoc `[this]` std::function wiring that is never unregistered. The new async support feature amplifies the pre-existing pattern.)

**Primary findings highlighted by architectural subagent**:
- Finding A (HIGH): `unknown_fill_handler` + general "install once, never clear" across engine/provider boundary. Handler runs on fill worker; accesses engine members; bridge outlives engine in observed ownership.
- Finding B (HIGH): Provider owns `bridge_` (shared_ptr<ExecutionBridge>); engine never calls `provider->close()`; destruction order relies on main.inc guards that are best-effort and bypassed.
- Finding C (HIGH): ObjectPool deleters capture raw `this`; pooled shared_ptrs handed through rings; no explicit `~engine()`.
- Additional: Worker threads, watermark callbacks, QuestDB string_view/ownership, shutdown sequencing gaps, watchdog liveness atomics.

Architectural recommendation (condensed): invert or strictly bound ownership for live resources; add explicit clear/unregister paths for every installed callback; add `~engine()` with assertions; prefer revocable handles over raw `std::function` lifetime coupling.

Both subagents were fresh, used different perspectives, received the truetest-core focus list (engine/workers/rings/pools/execution/questdb/binance-safety/callbacks/string_view + the new async_support change), and produced actionable structured output without any source modification.

## Detaillierte Findings

**HIGH-01**  
Datei: `src/engine/engine.cpp:132` (and surrounding ctor wiring ~77-180)  
Beschreibung: `unknown_fill_handler` lambda captures raw `this`, is stored permanently in `ExecutionBridge` (provider-owned), and is invoked from the fill-transport worker thread (`dispatch_unknown_fill`). The lambda touches `exit_manager_` (non-thread-safe for concurrent access from transport thread) and constructs `fill_event` / `synth_result`. No clearing of the handler occurs on any stop/halt/dtor path.  
Schwere: HIGH (UAF on live shutdown or late fill during unwind; data race on exit_manager state).  
Quelle: code reads, grep, both subagents, new async_support addition.

**HIGH-02**  
Datei: `src/types/object_pool.h:127` (acquire path) + engine members + ring handoff  
Beschreibung: Custom deleter `auto dtor_and_return = [this](T* p) { ... defer_release ... }` captures the pool. `shared_ptr<T>` values (events, orders, fills) are stored by value inside `RingBuffer<event_pointer>` and can be held by worker threads, UI snapshots, QuestDB sink, etc. If any shared_ptr outlives its owning pool (default dtor order, MC reuse, late ring drain), the deleter runs on a destroyed pool.  
Schwere: HIGH (UAF on pool internals / control block pool).  
Quelle: object_pool full read, ring_buffer, engine.h member decls, subagent 2.

**HIGH-03**  
Datei: `src/providers/binance/binance_futures_provider.h:684` (bridge_), `578` (close only joins internal transport), `src/engine/engine.cpp` (no provider->close calls), `src/bin/main.inc` (guard + release)  
Beschreibung: `ExecutionBridge` (and its fill transport + submit threads) is owned by the provider. Engine installs multiple `[this]` callbacks and handlers into provider objects during ctor. Engine does not own or reliably close the provider. Destruction order is fragile.  
Schwere: HIGH (dangling callbacks + threads touching engine state during/after engine dtor).  
Quelle: provider + bridge + main.inc reads, subagents.

**MEDIUM-01 to MEDIUM-04** (summarized): Worker thread `[this]` spawns + stop ordering, watermark_cb_ in ring (invoked from producer), multiple binance transport/DMS/user-data `[this]` threads, QuestDB ILP writer + store connection vs engine lifetime + string_view usage inside builders.

**LOW / style**: Clang-tidy naming/magic-numbers/special-members in pools and rings; valgrind mismatches in static registries (gtest/boost); no explicit engine dtor.

## Phasenbasierter Remediation-Plan

**WICHTIG:** Dieser Abschnitt enthält konkrete, in Phasen unterteilte Handlungsanweisungen für einen späteren Grok Build Agent mit Schreibrechten. Jede Phase hat ein klares Ziel, nummerierte Schritte, Verifikation und (wo sinnvoll) Beispiel-`search_replace` mit realistischem Kontext aus den analysierten Dateien.

### Phase 1: Vorbereitung & Setup (keine Logik-Änderungen)
1. Erzeuge einen dedizierten Build für Analyse:  
   ```bash
   cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON \
     -DENABLE_ASAN=ON -DENABLE_UBSAN=ON -DENABLE_BINANCE=ON -DENABLE_QUESTDB=ON
   cmake --build build-asan --target truetest_tests -j$(nproc)
   ```
2. Wiederhole für TSAN (separater Baum): `-DENABLE_TSAN=ON`.
3. Führe die drei Gate-Skripte aus und bestätige sie weiterhin grün.
4. Führe HotpathAllocs + Engine/Bridge/Worker/QuestDB Filter auf dem nicht-sanitized Build aus und notiere aktuelle Zahlen.
5. Erzeuge `compile_commands.json` (schon über Export in asan Build vorhanden).

**Verifikation am Ende der Phase:** `build-asan/truetest_tests` existiert und kann mit ASAN_OPTIONS gestartet werden; alle check-*.sh passieren.

### Phase 2: Kritische Sofort-Fixes (Callback Lifetime + Shutdown Ordering)
**Ziel:** Verhindern, dass Callbacks nach Zerstörung des Empfängers feuern können. Fokus auf den neuen async Pfad + allgemeines Pattern.

**Issue HIGH-01 + HIGH-03**  
Datei: `src/engine/engine.cpp` und `src/execution/execution_bridge.h`

**Konkrete Schritte:**

1. Füge eine klare Methode zum Zurücksetzen des Handlers hinzu (in Interface + Impl).

   In `src/execution/async_support.h` (Interface) und `src/execution/execution_bridge.h`:

   ```diff
   + virtual void clear_unknown_fill_handler() = 0;
   ```

   In ExecutionBridge impl:
   ```cpp
   void clear_unknown_fill_handler() override {
       std::lock_guard<std::mutex> lk(handler_mu_);
       unknown_fill_handler_ = {};
   }
   ```

2. Rufe den Clear in allen relevanten Stop-Pfaden des Engines auf (vor worker joins und vor Verlassen von run_*).

   In `src/engine/engine.cpp`, in `stop_workers()` (früh) und in `trigger_halt` Pfaden sowie am Ende von `run()` vor `stop_workers()`:

   ```diff
   + if (auto adapter = config_.provider ? config_.provider->get_execution_adapter() : nullptr) {
   +     if (auto* cap = adapter->get_async_support()) {
   +         cap->clear_unknown_fill_handler();
   +     }
   + }
   ```

3. Analog für andere in der Engine installierten Callbacks (event_publisher, funding factory, halt_cb auf provider) — setze sie auf leere Funktoren oder null wo möglich.

4. Stelle sicher, dass `provider->close()` aufgerufen wird (oder ein neues `detach()`), bevor der Engine-Dtor läuft. Mindestens in `stop_workers` für live mode und in allen run_* Exit-Pfaden.

   Beispiel-Kontext (engine.cpp stop_workers oder run Ende):
   ```diff
   if (config_.mode == engine_mode::live) {
       if (config_.provider) config_.provider->close();
   }
   ```

5. Baue mit ASAN und führe aus:
   ```bash
   ASAN_OPTIONS="halt_on_error=1:abort_on_error=1" \
     ./build-asan/truetest_tests --gtest_filter='*EngineAsyncSupport*|*BridgeUnknownFill*|*Engine*'
   ```
   Ergänze manuelle Shutdown-Tests (z.B. Signal während laufender bracket fills simulieren).

**Verifikation:** Handler ist nach stop leer; keine UAF unter ASAN bei Shutdown-Interleavings; Tests für async support erweitern um explizite "install → stop → inject late fill → assert no crash / no handler called".

### Phase 3: Ownership & Lifetime Modernisierung (Pools + Provider/Engine Boundary)
**Ziel:** Pools und Callbacks überleben nicht ihre Owner; Ownership wird explizit.

**Issue HIGH-02 + C**

1. Füge expliziten `engine::~engine()` hinzu (in engine.h + engine.cpp). Darin:
   - Alle installierten Callbacks clearen.
   - Provider close (falls noch nicht geschehen).
   - Assertions auf in_use() == 0 für alle Pools + leere Ringe (Debug-Builds).
   - Stoppe watchdog explizit.

2. Für ObjectPool: Erwäge eine der folgenden nachhaltigen Varianten (wähle eine):
   - Pools zu `shared_ptr<ObjectPool<T>>` machen und Deleter über `weak_ptr` gehen (Kosten beachten).
   - Oder: explizites "drain_and_poison()" vor Dtor, das alle ausstehenden shared_ptrs ungültig macht und deleters zu No-ops macht.
   - Oder: strikt dokumentieren + statische Analyse / Tests erzwingen, dass alle pooled Objekte vor Pool-Destruction released sind (aktueller Ansatz + Assertions).

   Beispiel für poison-Flag (object_pool.h):
   ```diff
   + std::atomic<bool> poisoned_{false};
   ...
   + if (poisoned_.load()) { /* free directly or abort in debug */ return; }
   ```

3. Erwäge Ownership-Inversion: Engine besitzt den Provider (oder einen "ExecutionSession") für seine Lebensdauer statt umgekehrt. Oder führe ein explizites `attach_engine` / `detach_engine` auf dem Provider/Bridge ein.

**Verifikation am Ende:**
- Neuer `~engine()` existiert und wird von Tests und live binaries durchlaufen.
- ASAN + TSAN laufen ohne neue Fehler auf erweiterten Shutdown-Tests.
- `test_engine_async_support.cpp` + neue Shutdown-Tests decken late-callback Szenarien ab.

### Phase 4: Concurrency & Atomics + Worker/Ring Härten
1. Audit alle `watermark_cb_` und anderen std::function in RingBuffer — entweder thread-safe machen oder nur vom Engine-Thread aus aufrufen.
2. ExitManager-Zugriffe aus dem unknown_fill_handler über eine Queue oder Mutex schützen (oder die Arbeit auf Engine-Thread marshallen).
3. WorkerWatchdog + DMS liveness atomics: sicherstellen, dass Quellen nicht freigegeben werden bevor Watchdog gestoppt ist (bereits teilweise dokumentiert).
4. Füge TSAN-spezifische Tests oder Stress-Tests für cross-thread fill + poll + stop hinzu.

### Phase 5: Verification & Re-Check (obligatorisch)
1. Baue neu mit ASAN+UBSAN (und separat TSAN).
2. Führe aus:
   - `./build-asan/truetest_tests --gtest_filter='*HotpathAllocs*|*Hotpath*'|*Engine*|*Bridge*|*Worker*|*QuestDB*|*Risk*'`
   - Die drei `scripts/check-*.sh`
   - Valgrind auf fokussierten Filtern.
3. Starte **diesen Skill erneut** mit demselben Pfad (`/memory-checks check-ups` oder dem vom User gewählten Verzeichnis).
4. Vergleiche neuen Report mit diesem. Keine HIGH Findings mehr.
5. Für Live-Safety Surface Änderungen: zusätzlich manuelle 4h shadow run + CCB (wie in docs/todos und freeze notes beschrieben).

### Phase 6: Dokumentation & Suppression Management
1. Ergänze Kommentare in engine.cpp / execution_bridge.h / object_pool.h mit expliziten Lifetime-Invariants und Shutdown-Sequenz.
2. Bei Bedarf: Suppressionen für bekannte false-positive in clang-tidy / valgrind nur für third-party Registry-Dtors.
3. Aktualisiere docs/governance oder todos mit den Ownership-Regeln ("keine rohen this-Captures über Provider/Engine Boundary ohne Revoke-Mechanismus").
4. Erweitere Test-Suite um explizite "late callback after stop" und "pool object lifetime after engine dtor" Fälle (dürfen nicht crashen und Handler dürfen nicht feuern).

**Abhängigkeiten zwischen Phasen:** Phase 2 vor 3; Phase 5 wiederholt sich nach jeder substantiellen Änderung. Phase 1 ist Voraussetzung für alle dynamischen Checks.

## Abschließende Hinweise
- Der Report wurde ausschließlich unter `check-ups/2026-07-16-memory-check.md` geschrieben. Keine Quelldatei, kein CMake, kein git-Index wurde verändert.
- Mindestens zwei frische Subagents mit unterschiedlichen Perspektiven wurden erfolgreich ausgeführt und ihre Ergebnisse integriert.
- Nächster Schritt für einen Build-Agent: Phase 1 Setup + Phase 2 Sofort-Fixes umsetzen und dann diesen Check erneut ausführen.

---

**Erstellt durch:** memory-checks Skill (read-only)  
**Datum:** 2026-07-16
