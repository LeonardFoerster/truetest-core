# CMake Modernization Design (2026-07)

**Context**: `/cmake-update` invocation. Full Phase 0 read-only multi-subagent assessment completed 2026-07-16 (see session transcript for the three subagent reports: CMake Drift & Structure, Project Status + Open Work, Configuration Matrix & Verification Ritual).

**Status of this doc**: Design implemented (Waves 1-2 + fixes + docs hygiene). All non-negotiable invariants re-verified after list completeness fixes. See session transcript for ritual output. Last updated during autonomous completion of /cmake-update.

> **Superseded invariant (2026-08 R4):** The historical requirement below that
> every option combination preserve three binaries and identical packaging now
> applies to the default/live-capable profile. `TRUETEST_RESEARCH_ONLY=ON`
> intentionally produces only Backtest and Shadow and restricts install/CPack;
> see `docs/reference/01-instructions.md`.

## Goals (Non-Negotiable Invariants)

- Exact same binaries and behavior for every combination of options (three `TT_TARGET` binaries, `engine_core` OBJECT library with PUBLIC propagation of `HAS_*` and optional sources).
- All FetchContent pins, generated `tt/truetest_version.h`, install, CPack, `web_assets` target remain identical in effect.
- C++23, per-config optimization, sanitizer mutual exclusion, and `tt_apply_*` helpers stay in place.
- Respect Phase 1 Live-Safety Freeze (`docs/governance/02-prerequisites.md`). Build changes that affect engine wiring or the LOC guard follow the token + CCB + clean shadow ritual where applicable. Always run `./scripts/check-live-safety-freeze.sh`.
- Zero new hot-path allocations or JSON introduced by build changes.
- Full test matrix + gate scripts (`check-hotpath-json.sh`, `check-layer-deps.sh`, `check-live-safety-freeze.sh`) must pass for every exercised configuration.
- **Net maintainability win**: fewer places to touch when a new `.cpp` or test is added. Adding a strategy or provider register should require touching **one obvious location** (plus the obvious test entry).

## Current Problems (from Phase 0)

- Monolithic 386-line root `CMakeLists.txt` containing giant explicit lists (`ENGINE_CORE_SOURCES` ~40 entries, `TEST_SOURCES` ~100+ entries).
- Source registration lives far from the code. No `add_subdirectory`, no component CMake files.
- Conditional wiring (`tt_wire_optional_backends`, `tt_wire_rich_tui`) uses repeated `${CMAKE_SOURCE_DIR}/` absolute paths (31 instances) and lives in `Dependencies.cmake`.
- One orphan (`src/web/tools/dump_fixtures.cpp`).
- One incorrect source (`.h` file added under Binance).
- `CMakePresets.json` is minimal — no first-class support for common real combos.
- LOC regression guard is a crude `file(STRINGS)` inside the `BUILD_TESTS` block.
- Adding anything requires editing multiple distant places + docs in several files.
- High risk of future drift between disk sources and build lists.

## Target Architecture

### File Layout (after modernization)

```
CMakeLists.txt                  # Short, declarative (options + high-level targets + includes)
cmake/
  CompilerFlags.cmake           # Unchanged role (flags policy, tt_apply_*)
  Dependencies.cmake            # FetchContent + tt_fetch_* + tt_wire_optional_backends (cleaned)
  Sources.cmake                 # NEW: canonical source lists + registration helpers
  truetest_version.h.in
CMakePresets.json               # Expanded with useful configure presets for common combos
```

Optional future (only if value is high):
- `cmake/components/Strategies.cmake`, `Simulation.cmake`, etc. (small includable fragments).

**Root `CMakeLists.txt` becomes short** (~100 lines or less):
- Options (unchanged)
- `include(CompilerFlags)`
- `include(Dependencies)`
- `include(Sources)`
- `tt_fetch_dependencies()`
- Version header generation (keep logic or extract to tiny helper)
- `add_library(engine_core OBJECT)`
- Call to populate engine_core from the canonical lists
- Factory for the three `engine_*` binaries
- `tt_wire_rich_tui` calls (unchanged)
- `if(BUILD_TESTS)` block (much thinner — delegates to Sources)
- Benchmarks, web_assets, shared lib, install, CPack (high-level)

### Sources.cmake (the key new file)

Responsibilities:
- Single source of truth for `ENGINE_CORE_SOURCES` (relative paths).
- Base `TEST_SOURCES` + the two conditional append blocks for QUESTDB/WEB.
- Grouped sections with clear comments (Core engine, Analytics, Data, Strategies, Simulation, Risk, Providers (local/synthetic), UI (console), Exits, Market Maker, etc.).
- Lightweight registration helpers (optional but valuable for future):
  ```cmake
  # Usage in future components or by hand:
  # list(APPEND ENGINE_CORE_SOURCES src/strategy/foo_strategy.cpp)
  ```
- A helper such as:
  ```cmake
  function(tt_populate_engine_core target)
      target_sources(${target} PRIVATE ${ENGINE_CORE_SOURCES})
  endfunction()
  ```
- The test list population logic (so the giant list lives in one file).

**Adding a new strategy** (target state):
1. Append the `.cpp` in the Strategies section of `cmake/Sources.cmake` (one obvious place).
2. Append the corresponding `tests/test_*.cpp` in the TEST_SOURCES section of the same file.
3. (Optional later) update any strategy registry if the new file uses the macro.

Net: 1-2 edits in one file instead of scattered edits across root CMakeLists + docs.

**Conditionals**:
- Move the `if(ENABLE_QUESTDB)` / `if(ENABLE_WEB)` append logic into `Sources.cmake` (still gated on the same options).
- Keep the heavy lifting of `target_sources` + `target_compile_definitions(PUBLIC HAS_*)` + `find_package` / FetchContent inside `tt_wire_optional_backends` in `Dependencies.cmake` (or extract small `tt_add_questdb_sources(target)` helper functions for readability).
- Absolute paths: normalize to relative where possible, or keep a consistent style with a comment explaining why `${CMAKE_SOURCE_DIR}` is used for conditional sources.

### Three Binaries and TT_TARGET

- Keep the existing `_tt_add_engine_binary` factory (it is already reasonably clean).
- `src/bin/main.inc` include pattern stays (per the deepdive design).
- Rich TUI wiring (`tt_wire_rich_tui`) stays exactly as-is (only on shadow + live).
- `tt_apply_live_flags` behavior is preserved exactly (even if docstring vs reality mismatch is noted for a separate cleanup).

### LOC Guard

- Move the guard into `cmake/Sources.cmake` or a new tiny `cmake/Guards.cmake`.
- Keep the exact same logic and failure mode.
- Call it from the `if(BUILD_TESTS)` block (or make it always evaluated when tests are on).
- Update the comment to point at this design + the engine-decomposition skill.
- Consider tightening `ENGINE_LOC_MAX` over time only after further decomposition.

### Generated Header and Other Invariants

- Version header generation logic can stay in root or be wrapped in a tiny function `tt_generate_version_header()`.
- `web_assets` custom target, install rules, CPack: unchanged.
- All FetchContent pins and `tt_fetch_*` functions: unchanged.

### CMakePresets.json Improvements

Add useful configure presets so common real-world invocations are first-class and documented:

```json
{
  "configurePresets": [
    ...existing...
    {
      "name": "linux-tests",
      "displayName": "Linux + Tests",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Linux" },
      "generator": "Unix Makefiles",
      "binaryDir": "${sourceDir}/out/build/${presetName}",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug", "BUILD_TESTS": "ON" }
    },
    {
      "name": "linux-binance-questdb",
      "displayName": "Linux + Binance + QuestDB + Tests",
      ...
      "cacheVariables": {
        "ENABLE_BINANCE": "ON",
        "ENABLE_QUESTDB": "ON",
        "BUILD_TESTS": "ON"
      }
    },
    {
      "name": "linux-web",
      ...
    },
    {
      "name": "linux-asan",
      ...
      "cacheVariables": { "ENABLE_ASAN": "ON", "BUILD_TESTS": "ON", "CMAKE_BUILD_TYPE": "Debug" }
    }
    // similar for TSAN, full-featured, Release + native, etc.
  ]
}
```

Document the new presets in `docs/reference/01-instructions.md` and README when the change lands.

### Documentation & Governance Hygiene

Any change that alters recommended cmake command lines or the surface of options **must** update in the same wave (or immediately follow-up):
- README.md
- `docs/reference/01-instructions.md`
- `docs/reference/02-user-manual.md`
- `docs/governance/04-summary.md`
- `docs/architecture/04-performance.md`
- Any phase0 scripts and operations docs that hard-code commands
- Cross references in todos if this closes a D-* item

After the work: run the "docs verified + links resolve + todo.md updated" ritual.

Reference the work in `docs/todos/06-D-documentation-structure.md` (and thin root) as appropriate.

### Phased Implementation Plan (Small, Reversible Waves)

Wave 1 (pure extraction, zero behavior change):
1. Create `cmake/Sources.cmake` containing the exact current `ENGINE_CORE_SOURCES` set + base `TEST_SOURCES` + the two `if(ENABLE_*) list(APPEND)` blocks + the LOC guard (moved).
2. In root `CMakeLists.txt`: `include(Sources)` early, remove the giant lists, keep using the same variable names.
3. Run the **full verification ritual** (see below).
4. Run all gate scripts.
5. Fresh subagent cross-review.

Wave 2 (clean helpers + grouping):
- Introduce section comments and thin helper functions in Sources.cmake.
- Possibly split strategies/simulation/etc. into commented blocks or tiny included fragments.
- Normalize path style comments.

Wave 3 (conditionals & Dependencies cleanup):
- Extract small `tt_add_*_sources(target)` helpers inside Dependencies.cmake or a companion for readability.
- Reduce duplication of absolute paths where safe.
- Fix the `.h` source misuse if low-risk.

Wave 4 (Presets + docs):
- Expand `CMakePresets.json`.
- Update all hard-coded command examples.
- Update governance/todo references.

After every wave: full ritual + gates + at least one fresh reviewer subagent.

### Full Verification Ritual (Mandatory After Every Wave)

Minimum matrix (from Phase 0 Configuration Matrix subagent + skill definition):

1. Configure + build (at least two generators where available):
   - `cmake -B build -DBUILD_TESTS=ON`
   - `cmake --build build -j1`
2. Common real combinations:
   - `-DENABLE_BINANCE=ON -DENABLE_QUESTDB=ON -DBUILD_TESTS=ON`
   - `-DENABLE_WEB=ON -DBUILD_TESTS=ON`
   - `-DENABLE_DEBUG=ON -DENABLE_BENCHMARKS=ON -DBUILD_TESTS=ON`
   - Sanitizers (`-DENABLE_ASAN=ON`, `-DENABLE_TSAN=ON`, `-DENABLE_UBSAN=ON`) + BUILD_TESTS (Debug)
   - `-DENABLE_NATIVE_OPT=ON -DCMAKE_BUILD_TYPE=Release`
   - `-DBUILD_SHARED_LIB=ON`
   - Full-featured mix
3. Test execution:
   - `ctest --test-dir build`
   - Spot-check conditional tests appear/disappear.
4. Gate scripts (must all pass):
   - `./scripts/check-hotpath-json.sh`
   - `./scripts/check-layer-deps.sh`
   - `./scripts/check-live-safety-freeze.sh --base ...` (as appropriate)
5. Explicit binary + target verification:
   - All three `engine_*` binaries exist and respond to `--help` / produce AUDIT line.
   - Rich TUI sources only for shadow + live.
   - `web_assets` target (when ENABLE_WEB + npm).
   - Install rules and generated header.
6. Cross-review: fresh subagent + check-work skill (or equivalent) on the CMake diff. Confirm no behavior change in any ENABLE_* path.
7. Documentation hygiene: example cmake lines in all referenced docs still work or were updated in the same change.

Only when every step is clean is a wave considered complete.

## Design Review Against Invariants (pre-implementation checklist)

- [ ] Still 100% explicit (lists of paths, no GLOB for sources) — YES in this proposal.
- [ ] Exact same binaries/behavior for all option combos + three TT_TARGET + engine_core OBJECT + PUBLIC propagation — preserved by construction (variables and call sites unchanged in effect).
- [ ] FetchContent pins, generated header, install, CPack, web_assets — untouched.
- [ ] C++23, flags, sanitizers, tt_apply_* — untouched.
- [ ] Phase 1 freeze respected (check script will be run; any engine.cpp impact would require token — none planned here).
- [ ] Zero new hot-path alloc/JSON from the build — this change only moves lists.
- [ ] Full matrix + gates after every wave — mandated.
- [ ] Net maintainability win — primary goal (one canonical Sources.cmake).
- [ ] Root becomes short and declarative.
- [ ] Adding strategy/provider/test becomes dramatically cheaper (1-2 edits in one file).

## Open Risks / Notes

- The absolute path usage in conditionals: we will reduce where possible but will not change semantics.
- `main.inc` include pattern and TT_TARGET story: left unchanged (architectural decision from prior decomp).
- Orphan `dump_fixtures.cpp`: leave as-is or add a clear comment (out of scope to suddenly build it unless requested).
- Native opt docstring vs reality mismatch: note for later; do not change behavior in this work.
- Any change that touches wiring of frozen surface files will require the full LIVE_SAFETY_CCB_APPROVED process (we will avoid that in this modernization).

## Success Criteria (when the whole effort is done)

- Root CMakeLists.txt is visibly short and declarative.
- A new core `.cpp` (strategy, etc.) or its test requires dramatically fewer manual edits.
- All previously working flag combinations and the three binaries continue to build and behave identically.
- The full verification matrix passes cleanly on the final state.
- Gate scripts green.
- Docs are consistent ("docs verified + links resolve + todo.md updated").
- A fresh reviewer can understand the build wiring in minutes.
- `git status` clean except the intentional modernization commits.

---

**Next step after design approval**: Implement Wave 1 (pure extraction of lists into `cmake/Sources.cmake`) as a small, reviewable, fully-reversible change, followed immediately by the complete verification ritual.
