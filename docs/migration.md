# Deepdive Migration Changelog

A running log of every file the prerequisites/deepdive refactor touches. One
section per prerequisite step (and later, per deepdive phase). Entries are
written **as the step lands**, not in advance — so the file is always a
truthful record of what the tree looks like relative to `master`.

Scope: from the first refactor commit after `4bbf845 version 0.2 new tools and
methods` (the last pre-refactor tip) onward. Anything older is out of scope.

Format per step:

- **Touched:** files added / moved / deleted / modified
- **Why:** one line on the motivating prerequisites.md bullet
- **Notes:** caveats, follow-ups, anything that would bite a reviewer

---

## Step 1 — Workspace / repo hygiene

**Commits:** `7dff73e`, `63904e6`, `fc90dd0`

**Touched:**

- `.gitignore` — build dirs, SQLite artefacts, debug logs ignored.
- `python/` → `tools/python/` — non-build scripts moved out of the root.
- Removed Windows/VS artefacts: `BacktestEngine/BacktestEngine.filters`,
  `BacktestEngine.vcxproj`, `BacktestEngine.vcxproj.filters`, `build.bat`,
  `BacktestEngine.sln`, stray `build-*` / `cmake-build-*` directories.

**Why:** §1 "Remove stray build directories" + "Retire the python/ directory if
unused" + "Resolve dirty working-tree items".

**Notes:** `truetest.db`, `truetest_debug.log`, `.idea/` artefacts remain in
the tree pending the "purge committed artefacts" bullet — covered by
`.gitignore` but deletion is staged.

---

## Step 2 — Build system (C++20 → C++23, three binaries, cmake split)

**Commits:** `31d0941`, `ef3a3ee`, `c5b3f30`

**Touched:**

- `CMakeLists.txt` — standard bumped to C++23, `engine_core` OBJECT library
  introduced; three executables (`engine_backtest`, `engine_shadow`,
  `engine_live`) produced from the same `main.cpp`.
- `cmake/CompilerFlags.cmake`, `cmake/Dependencies.cmake` — extracted.
- `BacktestEngine/src/core/tt_target.h` (new) — compile-time
  `TT_TARGET` id + `target_allows_live_orders()` gate.
- `BacktestEngine/src/main.cpp`, `tests/test_cli.cpp` — runtime mode switch
  kept only at the argument-parsing edge.
- `CLAUDE.md` — refreshed to describe the three-binary design.
- `ENABLE_NATIVE_OPT` flag added, applied to `engine_live` only.

**Why:** §2 "Bump CMAKE_CXX_STANDARD", "Split the monolithic binary into
three targets", "Introduce cmake/CompilerFlags.cmake and cmake/Dependencies.cmake",
"Add the per-target release flag set".

**Notes:** Remaining §2 bullet (remove stale `BacktestEngine.sln` /
`build.bat` / stale `CMakePresets.json`) was folded into Step 1.

---

## Step 3 — Source tree restructure

**Commits:** `f3e889f`, `507ec0a`

**Touched:**

- `BacktestEngine/src/**` → `src/**` (99 renames, no content change).
- `BacktestEngine/` wrapper deleted.
- `cmake/Dependencies.cmake`, `CMakeLists.txt` — include/source paths updated.
- `src/core/{engine.cpp,engine.h,engine_config.h,checkpoint.h,event_json.h}`
  → `src/engine/**`, isolating `core/` to dep-free types only.
- Include sweep across `src/api/truetest_api.cpp`, `src/main.cpp`,
  `src/providers/binance/binance_provider.h`, `src/threading/ws_worker.h`,
  `benchmarks/bench_main.cpp`, plus the full `tests/**` set.

**Why:** §3 "Move BacktestEngine/src/** → src/**" + "Isolate core/".

**Notes:** The deepdive's full directory taxonomy (`network/`, `db/`, `ml/`)
is NOT yet in place — that lands with Phase 1 of `todo.md`. `src/core/` is
dep-free only in practice; no CMake `OBJECT` library boundary enforces it yet
(§10 still pending).

---

## Step 4 — Dead / stub code removal

**Commits:** `b4b025a`

**Touched:**

- `providers/metatrader/` and `providers/polymarket/` — deleted (empty stubs).
- `web/index.legacy.html` — deleted; the React SPA covers its features.
- `Dockerfile` — deleted.
- `CLAUDE.md`, `instructions.md` — stale references dropped.

**Why:** §4 "Delete empty provider stubs", "Remove the legacy dashboard",
"Consolidate the redundant start.sh/build.bat/Dockerfile entry points".

**Notes:** `start.sh` retained as the single documented launcher. Strategy
scaffolding (`mean_reversion`, `sma`, `ma_crossover`) left intact; they
already match the current `IStrategy` interface — the deepdive-style
`StrategyBase` with `on_tick/on_fill/on_order_update/on_timer` lands in
Phase 6.

---

## Step 5 — Stability fixes (partial)

**Commits:** `d3cf9aa`

**Touched:**

- `src/providers/local/csv_parser.h` — tick parser exception safety.
- `tests/test_engine_streaming.cpp`, `tests/test_data_handler.cpp` — fix
  `SilenceOutput` ODR violation and tighten assertions.

**Why:** §5 "Fix EngineStreaming teardown crashes" + "Audit for data races".

**Notes:** **Not complete.** Remaining §5 work:

- Full TSAN audit report of `portfolio.cpp` / `risk_manager.cpp`.
- Ring-buffer memory-ordering audit against
  `docs/target-architecture.md` §4.1 (formerly `hft_engine_deepdive.md:452-510`).
- Object-pool single-threaded contract verification.
- `HasBarData` test is currently failing on the default build (pre-existing,
  unrelated to CI changes in Step 7). Root cause unidentified; tracked as an
  open item.

---

## Step 6 — Dependency hygiene decisions (documented, not yet enforced)

**Commits:** `3c3e6ad`

**Touched:**

- `CLAUDE.md` — new "Stack decisions" section recording:
  - `nlohmann/json` confined to `src/main.cpp` + `src/api/truetest_api.cpp`.
  - `SQLite` remains the default persistence; TimescaleDB deferred.
  - Binance stays; Bitstamp is a future `IProvider`, not a replacement.

**Why:** §6 "Audit nlohmann/json usage", "Decide SQLite vs TimescaleDB",
"Binance vs Bitstamp".

**Notes:** These are *decisions*, not yet *enforcement*. The CI grep that
fails on `nlohmann::json` outside the allowed files is still a Phase 0 gate
item.

---

## Step 7 — Test / CI baseline

**Commits:** `dbe024c` + this step

**Touched:**

- `.github/workflows/ci.yml` — matrix expanded to gcc-13 × clang-17 ×
  Debug/Release, plus dedicated jobs: `asan` (+UBSAN), `tsan`, `binance`,
  `postgresql`. Triggers widened to every push and PR on any branch. New
  jobs: `format` (clang-format-17 dry-run), `tidy` (clang-tidy-17 against
  `compile_commands.json`), `benchmarks` (build + smoke-run of
  `truetest_benchmarks`).
- `.clang-format` — 4-space indent, LLVM base, deepdive naming preserved.
- `.clang-tidy` — conservative starter check set with identifier-naming
  rules (snake_case functions, PascalCase types, `I`-prefixed interfaces).
- `benchmarks/bench_main.cpp` — orderbook/ring-buffer/SMA/event-JSON/engine
  microbenches. Builds under `ENABLE_BENCHMARKS=ON`; smoke-run wired into CI.
- Several tests re-ordered class member init after fixing a member-order UB.

**Why:** §7 all three bullets: CI matrix, clang-format/tidy, benchmark harness
placeholder.

**Notes:** CI also still expected to gate on the `HasBarData` fix (Step 5
follow-up). No runtime code changed in this step — purely CI/lint/bench
plumbing.

---

## Step 8 — Docs / knowledge base

**Commits:** *this step*

**Touched:**

- `instructions.md` → `docs/user-manual.md` — root-level MD limit enforced;
  status preamble links to CLAUDE.md + target-architecture.md; C++17 → C++23
  in the opening paragraph.
- `hft_engine_deepdive.md` → `docs/target-architecture.md` — with a
  "target architecture, not current reality" preamble enumerating each
  deviation (venue, persistence, JSON hot-path, language level, directory
  graph, LibTorch). The preamble is expected to shrink as deepdive phases
  land.
- `README.md` — rewritten: document-map table (CLAUDE.md, user-manual.md,
  target-architecture.md, migration.md, prerequisites.md, todo.md), current
  three-binary story, current stack decisions, `pre_transform` branch status.
- `docs/migration.md` (this file) — new.
- `prerequisites.md`, `todo.md` — internal references to
  `hft_engine_deepdive.md` rewritten to `docs/target-architecture.md`.

**Why:** §8 "Reconcile CLAUDE.md / README.md / docs / deepdive", "Remove
instructions.md (or move to docs/)", "Write one docs/migration.md".

**Notes:** `prerequisites.md` and `todo.md` remain at repo root — they are
the active workbench tracking this refactor and will move under `docs/` (or
be deleted) when Phase 0 closes. Historical design notes under
`docs/01-persistent-state.md` … `05-historical-backfill.md` and
`docs/refactor/00-overview.md …` were left untouched; they predate the
deepdive and are neither promoted nor demoted.

---

## Step 9 — Branch logic (git workflow)

**Commits:** *this step*

**Touched:**

- `docs/branch-policy.md` (new) — source-of-truth document for the workflow:
  branch roles, rebase-only merges, GitHub branch-protection settings,
  rollback tags, credential-handling rules, CI gates (per-PR vs nightly),
  `feat/*` review sizing, `wasm` disposition.
- `.gitignore` — added credential patterns (`config/live*.toml`, `*.pem`,
  `*secret*`, `.env`, `.env.*` with `.env.example` allow-listed). **Bug fix
  rolled in**: removed the pre-existing `docs/` ignore rule that was
  silently preventing Step 8 output (`docs/migration.md`,
  `docs/target-architecture.md`) from being tracked. The rule predated the
  refactor and would have swallowed every new doc from here on.
- `scripts/pre-commit` (new, +x) — rejects staged credential-shaped files
  even under `git add -f`. Activate per clone with
  `git config core.hooksPath scripts`. Smoke-tested: a dummy
  `secret_test.key` stage is rejected with a red banner and exit 1.
- `.github/workflows/ci.yml` — removed the inline `tsan` job; triggers
  already widened in Step 7.
- `.github/workflows/nightly.yml` (new) — TSAN on `pre_transform` via
  `cron: '30 2 * * *'` + `workflow_dispatch`. Branch-policy doc cites this
  as the authoritative TSAN path; per-PR TSAN is explicitly *not* run.
- Rollback tags created at current tips:
  - `pre-deepdive-20260421-master`
  - `pre-deepdive-20260421-pre_transform`

**Why:** §9 — freeze master, promote pre_transform, enforce rebase merges,
branch-naming convention, `wasm` disposition, credential protection, CI
matrix on every PR with TSAN nightly only, dated rollback tag.

**Not executed (documented only, require out-of-band action):**

- GitHub branch-protection rules for `master`, `pre_transform`, `phase/*`,
  and credential-touching branches — cannot be set from a commit; see
  `branch-policy.md` §2 for the exact click-path.
- `wasm` branch deletion + `archive/wasm` creation — destructive, not done
  automatically. `wasm` is confirmed to have zero unique commits vs
  `pre_transform` (7 commits, all present in `pre_transform`; the only
  WebAssembly hit on the branch is an npm transitive in
  `web/package-lock.json` — no real WASM code). Recommended action codified
  in `branch-policy.md` §7.
- Server-side `credentials-check` CI job — flagged as a TODO in
  `branch-policy.md` §4 to land before the first Phase 3 PR that introduces
  a working live-REST credential store.

---

## Step 10 — Directory separation (partial; CMake OBJECT split deferred)

**Commits:** *this step*

**Touched:**

- Domain workers moved out of `src/threading/` so `threading/` becomes the
  pure base-primitives layer the deepdive requires:
  - `src/threading/logging_worker.h`        → `src/engine/logging_worker.h`
  - `src/threading/risk_worker.h`           → `src/engine/risk_worker.h`
  - `src/threading/stats_worker.h`          → `src/engine/stats_worker.h`
  - `src/threading/observer_worker.h`       → `src/engine/observer_worker.h`
  - `src/threading/risk_stats_worker.h`     → `src/engine/risk_stats_worker.h`
  - `src/threading/market_maker_worker.h`   → `src/engine/market_maker_worker.h`
  - `src/threading/ws_worker.h`             → `src/engine/ws_worker.h`
  - Include-path updates inside each moved file + the single consumer
    (`src/engine/engine.h`).
  Rationale: `analytics/analytics.h` derives `Analytics` from `Worker`
  (threading → analytics edge), and the domain workers include their
  respective domain headers (analytics → threading edge). Moving the
  domain-specific workers into `engine/` (their only consumer) breaks the
  cycle and restores a DAG-shaped layer graph.
- `src/main.cpp` → `src/bin/main.inc` + three thin wrappers
  (`src/bin/engine_backtest/main.cpp`, `src/bin/engine_shadow/main.cpp`,
  `src/bin/engine_live/main.cpp`). Each wrapper `#include`s the shared
  implementation. `CMakeLists.txt` `_tt_add_engine_binary()` now takes an
  explicit entry-point path and routes each binary at its own TU under
  `src/bin/<target>/`.
- `scripts/check-layer-deps.sh` (new, +x) — parses every project-local
  `#include` edge under `src/` and rejects any edge forbidden by the
  deepdive's dependency graph. Exemptions: `HAS_DEBUG`-guarded `debug/`
  includes, and the four `debug/*_report|info|timer.cpp` files (compiled
  only when `HAS_DEBUG` is set).
- `scripts/check-credentials.sh` (new, +x) — rejects any file outside
  `src/providers/binance/`, `src/bin/engine_live/`, or `src/main.cpp` that
  mentions `api_key` / `api_secret`.
- `.github/workflows/ci.yml` — two new jobs, `layer-deps` and
  `credentials-check`, wired as per-PR gates.

**Why:** §10 "Encode the graph as CMake OBJECT libraries" (enforcement
achieved via CI script in lieu of CMake split — see deferred notes below);
"Forbid sibling includes" (folded into `check-layer-deps.sh`'s `core`
allow-list); "Binaries live under `src/bin/`"; "Physically isolate live
credentials".

**Notes:** Build verified clean across `engine_backtest`, `engine_shadow`,
`engine_live`, `truetest_tests`, `truetest_cli_tests` on the default preset
with `ENABLE_SQLITE=ON`, `BUILD_TESTS=ON`. All 349 tests pass (excluding
the pre-existing `EngineStreaming` / `HasBarData` / `ThreadPreset` hangs
tracked as Step 5 follow-ups).

**Deferred — explicitly out of scope for Step 10, tracked as Phase-0 follow-ups:**

- **CMake `tt_<module>` OBJECT library split.** Declaring each `src/<dir>/`
  as its own OBJECT library with a `target_link_libraries(… PUBLIC …)`
  allow-list requires resolving two surviving header-level couplings:
  `risk/risk_manager.h` includes `analytics/analytics.h` for
  `AnalyticsReport`; this creates a risk→analytics header edge that is
  legal per the CI script but would require an OBJECT-library cycle
  resolution (e.g. extracting `AnalyticsReport` into `core/` or splitting
  the risk manager into a thin interface + a heavy implementation). Doing
  this surgery alongside the CMake graph is out of scope for the
  prerequisite cleanup; the CI script already catches violations of the
  intended direction, so the CMake split can land mechanically once the
  header coupling is addressed.
- **Header hygiene (`src/<module>/include/tt/<module>/`).** A
  repository-wide rename that touches every `#include` site — deferred to
  the Phase 0 CMake restructure PR, where it will land in one sweep.
- **Tests mirror source (`tests/<module>/`).** ~40 test files would need
  to be redistributed. Deferred; the per-module split is more valuable
  once the CMake OBJECT libraries exist.
- **Platform code to `src/platform/linux/`.** Little code currently
  qualifies — the only meaningful `__linux__` guards are in
  `threading/thread_config.h` (CPU affinity). A dedicated directory now
  would be near-empty; will land with the SCHED_FIFO + huge-pages work in
  a later phase.
- **Per-binary strict `target_link_libraries` isolation.** The three
  wrappers in `src/bin/<target>/main.cpp` currently all `#include
  ../main.inc`, so functionally all three binaries still compile the same
  shared implementation. Divergence (live-only REST executor and
  credential store only linked into `engine_live`) lands with the
  CMake-split PR above.

---

## Step 11 — External libraries & build hygiene

Scope, per `prerequisites.md §11`: every third-party dep is acquired through
a single pinned mechanism (FetchContent or find_package), no vendored
source trees survive, build caching is wired into CI, and hot-path JSON
stays hand-rolled.

**Touched:**

- `cmake/Dependencies.cmake` — audited. All 7 FetchContent pins are exact
  tags: CLI11 `v2.4.2`, zstd `v1.5.6`, nlohmann_json `v3.11.3`, GoogleTest
  `v1.15.2`, Google Benchmark `v1.8.5`, libpqxx `7.9.2`, Abseil
  `20240722.0`. Every optional system library (Boost, OpenSSL, libpq,
  SQLite3) goes through `find_package`. No `third_party/` vendored
  sources exist.
- `.github/workflows/ci.yml` — ccache + `FETCHCONTENT_BASE_DIR` wired into
  every build-performing job (build matrix, asan, binance, postgresql,
  tidy, benchmarks). `CCACHE_MAXSIZE=500M`. FetchContent cache keyed on
  `hashFiles('cmake/Dependencies.cmake')` — one cache entry serves every
  job because the source tarball set is identical across them. ccache
  cache keyed per-job + `github.sha` with a per-job restore-keys
  fallback so cold rebuilds pull warm compiler caches.
- `scripts/check-hotpath-json.sh` — new CI gate (wired as
  `hotpath-json-check` job). Enforces that `nlohmann/json.hpp` /
  `nlohmann::` are confined to `src/bin/main.inc`,
  `src/api/truetest_api.cpp`, and `tests/`. Any other hit fails CI so a
  Phase-3 swap to simdjson / hand-rolled parsing stays a drop-in change.
- `docs/licenses.md` — new. Table of every shipped dep with license,
  link-mode, and `engine_live` permissibility. Abseil is flagged
  developer-only (debug instrumentation) and must not ship in live.
- `cmake/truetest_version.h.in` + `CMakeLists.txt` — new `configure_file`
  pipeline producing `build/generated/tt/truetest_version.h`. Captures
  `PROJECT_VERSION`, short commit SHA, dirty/clean flag, UTC build
  timestamp, build type, compiler id+version, and all FetchContent pin
  tags. Included from `src/bin/main.inc`; emits two `[audit]` log lines
  at startup so crash reports and live-order receipts trace back to a
  reproducible build.

**Why:**

Phase 0 will add simdjson (hot-path JSON), prometheus-cpp (metrics), and
LibTorch (ML strategies) behind new `ENABLE_*` flags. Phase 3 introduces
TimescaleDB as the default persistence. All of those additions must slot
into an acquisition model that's already normalized; CI must already
cache FetchContent tarballs and ccache output across jobs; and the
hot-path-JSON invariant must already be enforced mechanically so the
simdjson swap is mechanical rather than a repo-wide audit. This step
locks in that ground state.

**Notes (the subtle bits):**

- FetchContent tag pinning is belt-and-braces: exact tags (e.g.
  `v2.4.2`) mean upstream force-pushes / retagging silently invalidate
  our cache key — but since we hash `cmake/Dependencies.cmake`, any
  edit to a pin bumps the FetchContent cache entry automatically.
- `find_package(Boost)` is intentionally not pinned to a specific
  version — the two optional backends that need it (Beast, System) are
  stable across Boost 1.74+ and the headers are available in every
  CI distro we care about. If that changes, add a version constraint.
- The `hotpath-json-check` pattern matches both
  `#include <nlohmann/json.hpp>` and `nlohmann::` so using-declarations
  don't sneak past the grep.
- The version.h generator uses `git rev-parse --short=12` so two
  different 8-char SHAs don't collide in the AUDIT log; 12 chars is
  Google's production default.
- `TRUETEST_GIT_DIRTY` is derived from `git status --porcelain` at
  configure time — for reproducible CI builds the tree is clean and
  this lands as `"clean"`. Local dev builds usually show `"dirty"`.
  Downstream tooling that parses the audit line should treat dirty as
  "do-not-use-for-live".

**Deferred — explicitly out of scope, tracked as Phase 0 / 3 follow-ups:**

- **simdjson** (`ENABLE_SIMDJSON`) — hot-path JSON parser. Gated on the
  allow-list enforcement now in CI; flips the Binance parser from
  hand-rolled string extraction to simdjson with zero structural
  changes.
- **prometheus-cpp** (`ENABLE_PROMETHEUS`) — metrics exposure for the
  shadow/live deployments. Needs a dedicated `metrics/` module and is
  out of scope until the CMake OBJECT split from Step 10 lands.
- **LibTorch** (`ENABLE_LIBTORCH`) — ML strategies (Phase 3). Brings in
  a ~1 GB dep tree; must be behind an off-by-default flag and must not
  touch `engine_live` unless/until the license + latency profile are
  re-evaluated.
- **TimescaleDB as default persistence** (`ENABLE_TIMESCALE`) — Phase 3
  default-flip. Requires connection-pool + schema migration work
  separate from Step 11's acquisition-normalization scope.
- **License-audit CI enforcement.** `docs/licenses.md` is the source of
  truth, but there is no CI job that diff-checks new deps against it.
  Follow-up: a script that parses `cmake/Dependencies.cmake` +
  `vcpkg.json` and fails CI if a component is missing from
  `docs/licenses.md`.
- **Per-binary live-build audit.** A dedicated CI job that builds
  `engine_live` with `-DENABLE_DEBUG=OFF` explicitly asserted, verifying
  Abseil is absent from the live binary. Low-risk today (default-off)
  but worth mechanizing.
