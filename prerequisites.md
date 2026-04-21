# Prerequisites — Cleanup & Refactor Before Deepdive Implementation

This is the **"clean the kitchen before you cook"** list. Each item below should land
*before* any new deepdive functionality is added. They are grouped by scope.
Each bullet is actionable and scoped small enough to be handed to Claude Code as
a single instruction.

---

## 1. Workspace / Repo hygiene

- [ ] **Remove stray build directories from the repo root.** There are currently
  `build/`, `build-bin/`, `build_binance/`, `build-nobnc/`, `build_qty/`,
  `build_shadow/`, `build_tsan/`, `build_webui/`, `cmake-build-debug/`,
  `CMakeFiles/`, `Testing/`, plus a stale `cmake_test_discovery_604d6e09ad.json`.
  Only one canonical `build/` should exist and it should be gitignored.
  - Claude Code: `Clean up all the out-of-tree build directories in the repo root. Leave only "build/" if present and add any missing build paths to .gitignore.`
- [ ] **Purge committed artefacts.** `truetest.db`, `truetest_debug.log`, and the
  already-deleted `truetest.db-shm` / `truetest.db-wal` shouldn't be tracked.
  - Claude Code: `Ensure truetest.db, truetest_debug.log, and all SQLite -shm/-wal files are gitignored and untracked. Finalize the deletions already staged.`
- [ ] **Retire the `python/` directory if unused** (deepdive mandates C/C++ only,
  no Python). Audit, then either delete or move to `tools/` as purely
  operational.
  - Claude Code: `Inspect python/ and report what it contains. If it is demo code only, delete it; otherwise move non-build scripts to tools/.`
- [ ] **Resolve dirty working-tree items** from `git status`: `.gitignore`,
  `.idea/vcs.xml`, `.dockerignore`, `.github/`. Commit or revert each on its
  own topic so the deepdive branch starts clean.

## 2. Build system upgrades (required by the deepdive)

- [ ] **Bump `CMAKE_CXX_STANDARD` from 17 → 20.** The deepdive relies on C++20
  features (designated initializers, `std::atomic<std::shared_ptr>` free
  functions, `<bit>`, `<span>`, concepts in places).
  - Claude Code: `In root CMakeLists.txt set CMAKE_CXX_STANDARD to 20, then build with -DENABLE_BINANCE=OFF -DENABLE_SQLITE=ON and fix any C++20 migration warnings/errors that surface.`
- [ ] **Add the per-target release flag set** the deepdive mandates
  (`-march=native -mtune=native -flto -funroll-loops -fomit-frame-pointer`) as
  an opt-in `ENABLE_NATIVE_OPT` flag, applied only to the live binary target,
  not to tests.
- [ ] **Split the monolithic `truetest` binary into three targets**:
  `engine_backtest`, `engine_shadow`, `engine_live`. Today `main.cpp` branches
  internally on modes — this creates the risk that backtest code gets linked
  against live credentials. Source files shared between them go into an
  `engine_core` static library.
  - Claude Code: `Refactor CMakeLists.txt and main.cpp so that backtest, shadow, and live modes produce three separate executables linking a shared engine_core OBJECT library. Replace the current runtime mode switch with compile-time target selection.`
- [ ] **Introduce `cmake/CompilerFlags.cmake` and `cmake/Dependencies.cmake`.**
  The root `CMakeLists.txt` currently owns everything — extract the
  `FetchContent` blocks and compiler flag logic.
- [ ] **Remove the parallel ad-hoc `build-*` solution files**
  (`BacktestEngine.sln`, `CMakePresets.json` if stale, `build.bat`). If Windows
  support is desired keep a single preset, not eight.

## 3. Source tree restructure

The deepdive expects `src/{core, network, execution, risk, strategy, db,
logging, ml}/` at the project root. The current layout buries everything in
`BacktestEngine/src/`. Two options:

- **Option A (lower risk):** keep `BacktestEngine/src/` but *rename* the
  subdirectories to match the deepdive's taxonomy (`core/`, `execution/`,
  `risk/`, `strategy/`, `data/` → split into `network/` and `db/`, …). Update
  include paths in one sweep.
- **Option B (clean slate):** move `BacktestEngine/src/` → `src/`, delete the
  `BacktestEngine/` wrapper.

Pick one, then:

- [ ] Claude Code: `Move BacktestEngine/src/** to src/** and update every include path and CMake target_sources() reference. Verify the default build still passes.`
- [ ] **Isolate `core/`.** The deepdive rule is *everything depends on core/,
  core/ depends on nothing*. Today `core/engine.cpp` pulls in
  `data/`, `execution/`, `orderbook/`, `analytics/`, etc. Either rename the
  current `core/` to `engine/` and create a **new** minimal `core/` containing
  only `order.hpp`, `types/price.h`, `types/object_pool.h`, `ring_buffer.h`,
  `clock.h`, or enforce the discipline via a dep-check script.
  - Claude Code: `Audit every #include in BacktestEngine/src/core/** and produce a dependency report. Anything in core/ that includes from data/, execution/, orderbook/ must move to a new engine/ namespace so that core/ is dependency-free.`

## 4. Dead / stub code removal

- [ ] **Delete empty provider stubs.** `providers/metatrader/` and
  `providers/polymarket/` are empty directories that the deepdive never
  mentions — they are pure noise right now.
  - Claude Code: `Delete providers/metatrader and providers/polymarket (both are empty) along with their README references in CLAUDE.md.`
- [ ] **Remove the legacy dashboard `web/index.legacy.html`** once the React
  SPA covers its features. Until then, flag it as legacy and stop building it.
- [ ] **Consolidate the redundant `start.sh`, `build.bat`, `Dockerfile`
  entry points** into a single documented launcher. Today the README, CLAUDE.md,
  and `start.sh` each describe different invocation flows.
- [ ] **Prune unused strategy scaffolding.** Audit
  `strategy/mean_reversion_strategy.*`, `sma_strategy.*`, `ma_crossover_strategy.*`
  against the deepdive's `StrategyBase` interface
  (`on_tick/on_fill/on_order_update/on_timer`). Anything that predates that
  interface should be updated or removed — do not leave dead implementations
  that don't compile against the target interface.

## 5. Known stability issues to fix first

- [ ] **Fix the EngineStreaming teardown crashes** called out in CLAUDE.md.
  You cannot run walk-forward validation or long shadow sessions on top of an
  engine whose streaming path crashes at shutdown.
  - Claude Code: `Reproduce the EngineStreaming test crashes noted in CLAUDE.md, identify the teardown-order bug, and fix it. Keep the fix narrow — no scope creep.`
- [ ] **Audit `portfolio.cpp` / `risk_manager.cpp` for data races** under the
  existing worker architecture. The deepdive's risk layer must be lock-free on
  the hot path; fix any current locks before layering on new logic.
  - Claude Code: `Run the test suite under -DENABLE_TSAN=ON and triage every TSAN warning. Produce a short report grouped by file.`
- [ ] **Verify SPSC ring-buffer memory ordering** in
  `threading/ring_buffer.h`. Compare to the deepdive's `acquire/release` pattern
  at `docs/target-architecture.md` §4.1 and fix any mismatch — this will become a
  load-bearing primitive across the whole system.
- [ ] **Confirm the existing object pool is thread-safe-by-construction.** The
  deepdive pool is single-threaded by design; the current `object_pool.h` must
  be reviewed with that contract in mind before it gets reused for `Order` and
  `Fill` allocations.

## 6. Dependency hygiene

- [ ] **Audit `nlohmann/json` usage.** The deepdive wants it off the hot path
  entirely (replaced by simdjson for parsing and hand-rolled snprintf for
  serialization). CLAUDE.md already claims the hot path is hand-rolled —
  *verify* this by grepping for `nlohmann` / `json::parse` under
  `core/`, `execution/`, `strategy/`, and `providers/binance/`.
  - Claude Code: `Grep for nlohmann::json, json::parse, and "#include <nlohmann/json.hpp>" across the entire src tree. Produce a table of every hit with a verdict: (a) hot path — must move to simdjson/snprintf, (b) config-time only — keep.`
- [ ] **Decide: SQLite vs TimescaleDB.** The deepdive is TimescaleDB-only.
  SQLite is a blocker for the production path but useful for unit tests. Keep
  SQLite gated behind `ENABLE_SQLITE` for tests only, and make the default live
  build require PostgreSQL+Timescale.
- [ ] **Binance vs Bitstamp.** The deepdive targets Bitstamp; this codebase
  targets Binance. Either (a) keep Binance and treat Bitstamp as a new provider
  (the cleanest path given the existing `IProvider` abstraction), or (b) rip
  out Binance and port to Bitstamp. **Decide and document this first** — it
  drives every Part 3 task.

## 7. Test / CI baseline

- [ ] **Enforce `-DBUILD_TESTS=ON` in CI on every push.** Current CI config
  under `.github/workflows/ci.yml` must cover at least: default build, Binance
  build, PostgreSQL build, ASAN build, TSAN build.
- [ ] **Add a clang-tidy + clang-format pass** before any new code lands.
  Pick the deepdive's style (4-space, trailing commas in C++20, `snake_case`
  free functions, `PascalCase` types) and codify it in `.clang-format`.
- [ ] **Add a benchmarking harness placeholder.** The deepdive's performance
  targets (p99 risk-check < 1µs, order path < 100µs software overhead) are
  meaningless without a benchmark to measure them. Wire up Google Benchmark
  under `ENABLE_BENCHMARKS` with one trivial bench so the plumbing exists.

## 8. Docs / knowledge base

- [ ] **Reconcile `CLAUDE.md`, `README.md`, `docs/`, and
  `docs/target-architecture.md`.** Today they describe different visions (modular
  backtester vs. production HFT engine). Either:
  - promote the deepdive to the authoritative spec, or
  - demote it to `docs/target-architecture.md` and annotate deviations from
    the current codebase.
- [ ] **Remove `instructions.md`** (or move to `docs/`) — root-level `*.md`
  files should be limited to `README.md`, `CLAUDE.md`, and the deepdive.
- [ ] **Write one `docs/migration.md`** that lists every file the refactor
  touches — this becomes the running changelog as the deepdive is implemented.

---

## 9. Branch logic (git workflow)

Current branch topology: `master`, `pre_transform` (checked out), `wasm`. The
deepdive is a multi-month refactor — merging it straight onto `master` in one
PR is suicide. Adopt the following discipline before the first new commit.

- [ ] **Freeze `master`.** `master` = currently-running Binance/SQLite
  backtester. No deepdive code lands there until a whole phase (per `todo.md`)
  is green in CI *and* passes a manual shadow run.
- [ ] **Treat `pre_transform` as the long-lived integration branch** for the
  deepdive. All phase branches merge here via PR; `master` pulls from
  `pre_transform` only at phase boundaries.
- [ ] **Rebase, don't merge, across phases.** Linear history makes `git
  bisect` actually useful when the hot path regresses by 30ns. Enforce
  `Rebase and merge` in GitHub branch-protection settings.
- [ ] **Branch-per-phase, not per-file.** One branch per `todo.md` phase
  (`phase/01-build`, `phase/02-memory`, …). Inside a phase, use short-lived
  topic branches that merge into the phase branch.
- [ ] **Naming convention:**
  - `phase/NN-short-name` — integration branches for each deepdive phase
  - `feat/NN-short-description` — features inside a phase
  - `fix/issue-NN` — bugfixes against `master` that need backporting
  - `refactor/NN-name` — prerequisites.md items only
  - `spike/name` — experimental; never merged, only cherry-picked
- [ ] **Decide what happens to `wasm`.** It has no obvious place in the
  deepdive's architecture (live HFT + TimescaleDB + LibTorch ≠ WASM). Either
  kill it, merge forward, or archive it as `archive/wasm` to keep it out of
  the PR list.
  - Claude Code: `Inspect the wasm branch. Summarise what it adds vs. master. Recommend: keep, archive, or delete.`
- [ ] **Tag the current tip before touching anything.** `git tag
  pre-deepdive-$(date +%Y%m%d) HEAD` on both `master` and `pre_transform`.
  This is the rollback point if the refactor goes sideways.
- [ ] **Protect credentials branches.** `live.toml`, API keys, and any branch
  that could hold them should have force-push disabled and required reviews.
  Add a `.gitignore` + pre-commit hook rejecting any file matching
  `config/live*.toml`, `*.pem`, `secret*`, `.env`.
- [ ] **Keep `feat/*` branches under ~500 LOC diff.** The deepdive phases are
  big, but each reviewable PR should not be. Break aggressively; stack PRs
  using `git-spice` or `gh pr create --base phase/…`.
- [ ] **CI matrix required on every PR:** default build, Binance build, PG
  build, ASAN, UBSAN, and tests. TSAN runs nightly on `pre_transform` only
  (it's slow). Phase branches cannot fast-forward into `master` without a
  green nightly TSAN in the previous 7 days.

## 10. Directory separation (the hard version)

The deepdive's §1.1 layout is not just aesthetic — it enforces a layered
dependency graph. Every layer below is allowed to include from layers further
up the list, never the other way. Violations are silent bugs waiting to
happen (circular includes, link cycles, test binaries that accidentally pull
in libpq).

**Allowed dependency direction (top → bottom, nothing goes back up):**

```
core/               ← types, allocators, ring buffer, clock. ZERO deps.
logging/            ← depends only on core/
network/            ← depends on core/, logging/. No strategy/execution knowledge.
db/                 ← depends on core/, logging/. No network/ code.
execution/          ← depends on core/, logging/, risk/, network/
risk/               ← depends on core/, logging/
strategy/           ← depends on core/, logging/, execution/ (intents only)
engine/             ← orchestrator; depends on everything above
ml/                 ← depends on core/, db/. NEVER linked into engine_live.
backtest/           ← depends on core/, strategy/, execution/. No network/.
tools/              ← CLI utilities; can depend on anything, never depended on.
```

- [ ] **Encode the graph as CMake `OBJECT` libraries**, one per directory.
  Each library's `target_link_libraries(… PUBLIC …)` names exactly the
  directories it is permitted to depend on. Any violation becomes a link
  error, not a convention document nobody reads.
  - Claude Code: `For each src/<dir>/, create an OBJECT library named tt_<dir> with its allowed dependencies declared via target_link_libraries(PUBLIC …). Do this in the same commit that moves the files; verify the default build still passes and that cycles produce a CMake error.`
- [ ] **Enforce header hygiene.** Public headers live in
  `src/<module>/include/tt/<module>/*.h`; private headers stay beside
  the `.cpp` file. `target_include_directories(tt_<module> PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include)` — private headers are unreachable
  from other modules.
- [ ] **Forbid sibling includes.** Add a clang-tidy `misc-include-cleaner`
  check and a CI script that fails if any file under `src/core/` includes a
  header from outside `src/core/` or the standard library.
- [ ] **Configuration is not a module.** Config structs live in `core/`
  (POD), parsing lives in `tools/config_loader/`, hot-reload lives in
  `strategy/config_manager.{h,cpp}`. Do *not* put a giant `config/` directory
  at project root — that invites every module to include it.
- [ ] **Tests mirror source.** `tests/<module>/` mirrors `src/<module>/`
  one-to-one. `tests/CMakeLists.txt` iterates `src/` subdirectories and
  creates one test binary per module so breakage is localized.
- [ ] **Platform code gets its own directory.** Linux-specific code
  (`SCHED_FIFO`, `MAP_HUGETLB`, `inotify`) lives in `src/platform/linux/`.
  Porting to BSD or macOS later means touching one directory, not fifty
  `#ifdef __linux__` blocks.
- [ ] **Binaries live under `src/bin/`.** Three files:
  `bin/engine_backtest/main.cpp`, `bin/engine_shadow/main.cpp`,
  `bin/engine_live/main.cpp`. Each has its own `target_link_libraries` list
  so that `engine_live` cannot be built with LibTorch or mock components and
  `engine_backtest` cannot be built with the REST executor.
- [ ] **Physically isolate live credentials.** `src/bin/engine_live/` is the
  *only* place a credential store may be included. Add a CI check: grep for
  `api_secret` / `api_key` patterns outside this directory and the config
  loader → fail.

## 11. External libraries (dependency catalog)

Current deps (confirmed from `CMakeLists.txt` + `vcpkg.json`):

| Library              | Purpose                     | Source              |
|----------------------|-----------------------------|---------------------|
| CLI11 v2.4.2         | arg parsing                 | FetchContent        |
| nlohmann/json        | JSON (static config only)   | FetchContent        |
| zstd v1.5.6          | event-log compression       | FetchContent        |
| libpqxx + libpq      | PostgreSQL                  | FetchContent/vcpkg  |
| Boost (headers+system)| Beast WS, asio             | find_package/vcpkg  |
| OpenSSL              | TLS for Binance             | find_package        |
| SQLite3              | test persistence            | find_package        |
| GoogleTest           | unit tests                  | FetchContent        |
| Google Benchmark     | perf tests                  | FetchContent        |
| Abseil               | debug instrumentation       | FetchContent        |

**New deps mandated by the deepdive, not yet present:**

| Library              | Purpose                           | Pin                     | Notes |
|----------------------|-----------------------------------|-------------------------|-------|
| simdjson             | hot-path JSON parsing (§3.4)      | v3.9.x                  | FetchContent; header+one-TU; no extra deps. |
| libtorch             | DL pipeline (§10.2)               | 2.1.0 CPU or 2.1.0 CUDA | ~200 MB download; pin via a cache script, NOT FetchContent. |
| prometheus-cpp       | /metrics endpoint (§11.1)         | v1.2.x                  | Pull `core`, `pull` components only; skip `push`. |
| libmicrohttpd (opt.) | serves /metrics if not embedded   | 0.9.x                   | Only if prometheus-cpp's pull handler isn't enough. |
| TimescaleDB          | server-side extension (§7)        | 2.14+                   | Not a C++ dep; document in deployment docs. |

- [ ] **Standardize on one acquisition mechanism per dep.** The current mix
  of `FetchContent` (CLI11, zstd, nlohmann, GTest, Abseil) plus `find_package`
  (Boost, OpenSSL, SQLite, PostgreSQL) plus `vcpkg.json` (for
  Postgres/Boost/OpenSSL features) is confusing and slow. Rule:
  - **Small header/source-only libs → `FetchContent` with pinned tag.**
  - **Large system libs (Boost, OpenSSL, PostgreSQL, libtorch) → `find_package`
    against system/vcpkg packages.** Never `FetchContent` libtorch or Boost.
  - Remove redundancy: if a dep is in `vcpkg.json` it should not also be a
    `FetchContent_Declare`.
  - Claude Code: `Audit every dependency in CMakeLists.txt and vcpkg.json. For each, decide FetchContent vs find_package per the rule above. Produce the diff in a single PR titled "refactor: unify dependency acquisition".`
- [ ] **Pin every `FetchContent_Declare` to an exact tag** (no `main`, no
  `master`, no floating branches). The current file already pins CLI11
  (`v2.4.2`), zstd (`v1.5.6`) — verify every new addition follows suit.
- [ ] **Vendor nothing.** No dep source code checked into this repo. If a
  library must be patched, maintain a fork and pin against it explicitly.
- [ ] **Build caching.** Deepdive-class deps (LibTorch especially) add minutes
  to cold builds. Add `ccache` guidance to `docs/build.md` and wire
  `CCACHE_DIR` into the CI runner. Use
  `FETCHCONTENT_BASE_DIR=${HOME}/.cache/ttsrc` so phase branches share
  downloads.
- [ ] **Security & license audit.** Run `cargo-deny`-equivalent for C++
  (e.g. `scan-build` + manual license check). The live binary must not ship
  GPL-linked code. LibTorch is BSD-3 — fine. simdjson is Apache-2.0 — fine.
  libmicrohttpd is **LGPL** — dynamically link, don't statically link.
- [ ] **Reproducible builds.** `hft_engine_version.h` generated at build time
  with: commit SHA, CMake cache hash, LibTorch version, simdjson tag. The
  live binary logs this at AUDIT level on startup so any production incident
  can be tied to an exact dep set.
- [ ] **Optional-dep matrix.** Keep each new dep behind an `ENABLE_*` flag
  (deepdive convention from CLAUDE.md):
  - `ENABLE_SIMDJSON` (default ON — blocks Binance build otherwise)
  - `ENABLE_LIBTORCH` (default OFF — only the ML binary needs it)
  - `ENABLE_PROMETHEUS` (default ON for shadow/live, OFF for backtest)
  - `ENABLE_TIMESCALE` (new flag; implies `ENABLE_POSTGRESQL`)
- [ ] **Remove Abseil** unless `ENABLE_DEBUG` justifies it. The deepdive
  doesn't use it; it's a heavy dep for something that only surfaces in debug
  reports today.
- [ ] **Kill `nlohmann/json` on the hot path for real.** After simdjson lands,
  the only `nlohmann::json` call sites allowed are: `tools/config_loader/`,
  `src/api/truetest_api.cpp`, tests. Enforce with a grep in CI.
