# TrueTest / hft-engine - Master Consolidated Instructions

**Status**: Single authoritative reference. Produced via multi-agent exhaustive extended-thinking analysis of every Markdown file in the repository. This document lives alongside a reorganized documentation set (see [docs/README.md](../README.md) for the current structure). It supersedes the scattered prior documents while preserving all key substance.

**Version**: 1.0 (Consolidated 2026-05 from 4 parallel deep-dive subagent reports + source cross-checks)  
**Philosophy**: "Someone reading only one phase still has the context of the invariants." Deliberately comprehensive and repetitive on safety. Update after every phase or material change.

**Quick Links (Internal)**: [Philosophy & Invariants](#philosophy-invariants) • [Production Phases](#production-phases) • [Build](#build-reference) • [CLI Reference](#cli-reference) • [MC / Providers / QuestDB usage](#providers-data-realism) • [Critical Warnings](#critical-warnings)

---

<a id="philosophy-invariants"></a>
## 1. Philosophy, Invariants, Safety Model

TrueTest is a modular C++23 engine for reproducible backtesting, divergence-aware shadow trading, and gated live execution from a **single source tree**. The default/live-capable profile builds three binaries (`engine_backtest`, `engine_shadow`, `engine_live`) that differ by the compile-time `TT_TARGET` define in `src/core/tt_target.h`; the Research-only profile omits the Live target. Live-order paths are physically removed via dead-code elimination in non-live targets (`target_allows_live_orders()` is constexpr false).

**Core non-negotiable invariants** (full authoritative list + rationale in [../governance/01-prod.md](../governance/01-prod.md); also AGENTS.md):

1. **Compile-time live-order gate is absolute** - Only `engine_live` (and future keeper_live targets) can ever emit real orders/transactions. Never introduce runtime `allow_live_orders` checks or recompile-time bypasses.
2. **Halt is terminal** (`halt_flag_` in engine/risk) - Write-once atomic true; only manual operator intervention + explicit process restart clears it. No auto-resume, no SIGUSR1, no cooldown, no "helpful" retry logic on safety paths.
3. **Safety paths are loud and non-retrying** - Kill-switch, DMS, reconciler, WorkerWatchdog failures escalate to operator with clear diagnostics. No silent backoff/retry on safety surfaces.
4. **Hot-path discipline** (enforced by `scripts/check-hotpath-json.sh` + layer-deps) - Zero `nlohmann::json` on hot path; zero (or object-pool) allocations on event loop; lock-free SPSC RingBuffer (65536 slots, exactly one producer/consumer); no second writer on any ring.
5. **Reconciler refusal is default** - Blocks startup on position/order drift > tolerance (configurable `--reconcile-tolerance-bps`). Only documented soft-warn exception: spot testnet monthly account resets (futures has none).
6. **User-data WebSocket is source of truth** (Binance futures/spot) - `ORDER_TRADE_UPDATE` + `ACCOUNT_UPDATE` after initial REST ack. Position snapshots from REST are advisory only until reconciled.
7. **DMS protects orders only** (`/fapi/v1/countdownCancelAll`) - Venue-side auto-cancel on heartbeat loss. It never emits reduce-only MARKET flattens itself. Its first heartbeat failure signals terminal halt; the engine-owned exact-once kill session performs the sole bounded cancel/flatten attempt.
8. **Futures mandates** - One-way mode hard refusal in
   `BinanceFuturesProvider::open()`; two non-atomic conditional algo legs with
   `closePosition=true` and no quantity/`reduceOnly`; no sibling-auto-cancel
   assumption; pre-trade venue `FuturesRiskCheck`
   (notional/leverage/liq-distance + real tiered MMR from
   `/fapi/v1/leverageBracket`) consulted **before** `RiskManager` in the hot
   path.
9. **Provider abstraction is the sole extension point** - `IProvider` + four safety hooks (`IReconciler`, `IKillSwitch`, `IRiskCheck`, `IBracketAdapter`) + transport/parser/executor. Core engine never contains `#ifdef HAS_*` or venue specifics.
10. **Small capital first + evidence-based gates** - Every phase exit requires artifacts (binary logs, QuestDB run_tag, signed notes, shadow reports) + two-person sign-off before capital tier increase. "No capital tier increase is permitted until all nine rows [Go-Live Gate] have two signatures."

**Additional strong primitives already present**: Layered risk (venue first), `WorkerWatchdog` (3× heartbeat), clock-skew/WAF/symbol existence/one-way probes at open, `ExecutionBridge` mutex audit, rate limiter, per-lot `Portfolio` + `ExitManager`, binary zstd event log + replay, QuestDB (soft-fail by default; strict fail-closed mode available), rich ncurses TUI, StageTimer/ring stats observability.

**Philosophy quote** ([../governance/01-prod.md](../governance/01-prod.md)): "The engine already implements strong compile-time and runtime safety primitives that most retail or early-stage systems lack... This playbook exists so that future operators cannot say 'we forgot why we were careful.'"

---

## 2. AI Coding Assistant Rules & Model Selection + Phase 1 Live-Safety Freeze

From AGENTS.md + [../architecture/02-model.md](../architecture/02-model.md) (full rationale):

- **Default**: Sonnet 4.6 sufficient for strategies, indicators, tests, CLI, docs, single-file refactors, new providers following patterns.
- **Switch to Opus 4.7 (`/model opus`) before touching**:
  - `src/engine/engine.{h,cpp}`, `engine_config.h`
  - `*kill_switch*`, `*dead_mans_switch*`, `*reconciler*`, `*watchdog*`
  - `src/core/tt_target.h` + any `TT_TARGET`/`target_allows_live_orders` callsites
  - `src/threading/` (SPSC, spin, affinity)
  - `src/risk/` + any `halt_flag_` code
  - Hot-path (no nlohmann/json)
  - Binance live safety glue (refusal gates, time sync, OCO/brackets, REST signing, DMS heartbeats)
- **Why**: These carry cross-file invariants (compile-time gating, terminal halt, no hot-path alloc/JSON, no auto-retry on safety). Sonnet has measurable tendency to add "helpful" fallback/retry logic that violates them.
- **Anti-patterns** ([../architecture/02-model.md](../architecture/02-model.md) explicit rejects): retry on kill-switch, resettable halt, hot-path JSON, runtime live-order check, HAS_* in core, second producer on SPSC, reconciler soft-warn for convenience, adaptive heartbeat.

**Phase 1 Live-Safety Freeze**: See [../governance/01-prod.md](../governance/01-prod.md) and [../governance/02-prerequisites.md](../governance/02-prerequisites.md). The exact safety surface is maintained in `scripts/check-live-safety-freeze.sh`; edits require the `LIVE_SAFETY_CCB_APPROVED` token, CCB review, and a clean multi-hour `engine_shadow` run.

**Pre-merge safety checklist** (see AGENTS.md + prod): model used, target gate not bypassed, halt terminal, scripts pass, no anti-patterns.

---

<a id="production-phases"></a>
## 3. Production Readiness Playbook & Capital-Tier Phases

Full authoritative details, Phase 0/1 gates, exact ritual, Go-Live table (9 rows requiring two signatures + evidence), and philosophy live in [../governance/01-prod.md](../governance/01-prod.md) (and `reports/phase0/` for evidence). This file focuses on technical how-to and pointers.

**Key Phase 0 command template** (see 01-prod.md for full "why" + exit criteria + ritual):
```bash
RUN_TAG="p0_$(date -u +%Y%m%d_%H%M)"
./build/engine_live \
  --provider binance-futures \
  --symbol BTCUSDT \
  --stream trade --depth-stream depth20@100ms \
  --live \
  --api-key "${BINANCE_FUTURES_KEY}" --api-secret "${BINANCE_FUTURES_SECRET}" \
  --log-events "./event_log_${RUN_TAG}.bin" \
  --persist --run-tag "${RUN_TAG}" \
  --reconcile-tolerance-bps 3 \
  --dead-man-countdown-ms 30000 --dead-man-heartbeat-ms 8000 \
  --max-notional 15000 --max-leverage 2.5 --min-liq-distance-pct 0.07 \
  --max-daily-loss 80 --risk-unwind
```
Status: 0/15 qualifying (see reports/phase0/PROGRESS.md + docs/governance/03-todo.md).

**Phase 1 Live-Safety Freeze**: the expanded engine/provider/execution safety surface requires the `LIVE_SAFETY_CCB_APPROVED` token + CCB + clean shadow run (see `docs/governance/01-prod.md`, `02-prerequisites.md`, and the exact list in `scripts/check-live-safety-freeze.sh`).

**Phases 2–6**: High-level roadmap in docs/governance/01-prod.md.

**Go-Live Gate**: See full 9-row table in docs/governance/01-prod.md. No capital tier increase without all + two signatures + evidence.

---

## 4. Prerequisites, Change Control, Task Tracking

See [../governance/02-prerequisites.md](../governance/02-prerequisites.md) for the living Phase 1+ checklist (must be green before PRs touching frozen surface). Run `./scripts/check-live-safety-freeze.sh` (optional `--base <commit>`), reference relevant items in [../governance/03-todo.md](../governance/03-todo.md) (or a specific item, e.g. `docs/todos/02-P1-freeze.md` P1-02). Historical plans/gaps live in `docs/archive/`.

(Planned architecture docs under ../architecture/ use "Planned for Doc Phase X – current details in docs/governance/01-prod.md + this file".)

**Note**: docs/ is now the central authoritative documentation home. Governance files live under docs/governance/. Last updated: 2026-07 (docs overhaul).

See [../governance/03-todo.md](../governance/03-todo.md) (thin high-level) for the phased task list or a specific item such as `docs/todos/01-P0-phase0.md` P0-01 (see docs/todos/00-OVERVIEW.md). Every frozen PR must reference relevant items by file and item ID.

reports/phase0/ contains the evidence machinery (README for layout, PROGRESS.md tracker, ops/ for batch reviews, templates/ for session notes). Current Phase 0 gates live in docs/governance/01-prod.md and docs/todos/01-P0-phase0.md.

---

<a id="build-reference"></a>
## 5. Build & CMake Reference (from instructions.md + AGENTS.md + perf docs)

**Canonical path:** CMake presets → `out/build/<preset>/`. Prefer **one warm tree** for daily work. Ad-hoc `build/` is legacy/one-off only — do not keep it warm in parallel with presets (each tree re-builds FetchContent `_deps/`, typically 0.5–2 GB).

**Minimal tests tree:**
```bash
cmake --preset linux-tests
cmake --build --preset linux-tests --target engine_backtest truetest_tests
ctest --preset linux-tests
```
This targeted command produces `engine_backtest` and `truetest_tests` under
`out/build/linux-tests/`. Omit `--target ...` to build all default targets,
including all three distinct `TT_TARGET` engine binaries.

**Research-only build and distribution boundary:**

```bash
cmake --preset linux-research-only
cmake --build --preset linux-research-only
ctest --preset linux-research-only
cmake --install out/build/linux-research-only \
  --prefix out/build/linux-research-only-install
cpack --config out/build/linux-research-only/CPackConfig.cmake
```

The preset explicitly sets `TRUETEST_RESEARCH_ONLY=ON`. In that configuration
CMake never defines `engine_live`, never compiles its exclusive
`src/bin/engine_live/main.cpp` entry point, and omits the binary from install
and CPack rules. A direct `--target engine_live` build therefore fails because
the target is unknown. This is a physical build/distribution boundary, not a
runtime setting. It supplements rather than replaces the unchanged constexpr
`target_allows_live_orders()` protection in `src/core/tt_target.h`.

The `research_artifact_guard` CTest checks the target list, direct target
failure, built binaries, `compile_commands.json`, staged install, install
manifest, and TGZ contents. Its negative companion injects a dummy
`engine_live`, a `TT_TARGET_LIVE` compile command, and a renamed wrapper into a
validated guard-owned build directory and proves rejection. A third test proves
that live-capable venue options and stale live build trees fail at configure
time.

Research CPack intentionally permits only the audited TGZ binary generator;
`-G DEB` overrides and source packages fail closed. The repository retains the
live implementation for explicitly live-capable builds, so a source archive is
not a Research distribution. The Core tree has no checked-in CI workflow;
external CI must run the `linux-research-only` configure/build/test sequence to
activate the three `build-security` CTests.

Use a clean, dedicated Research build tree. `ENABLE_BINANCE` and
`ENABLE_BITGET` are incompatible with Research-only because their current
shared provider registrations compile real order transports into
`engine_core`; CMake reports a fatal conflict instead of silently weakening the
boundary. Local/synthetic and shadow-only Bitunix remain available.

`TRUETEST_RESEARCH_ONLY` defaults to `OFF` to preserve the prior three-binary
build. Request that mode explicitly when live development is intended:

```bash
cmake --preset linux-tests -DTRUETEST_RESEARCH_ONLY=OFF
cmake --build --preset linux-tests --target engine_live truetest_cli_tests
ctest --preset linux-tests
```

**Source registration:** Core and test source lists live in `cmake/Sources.cmake` (the single obvious place to register a new strategy, simulation component, or test). Optional venue/backend `.cpp` files are wired in `cmake/Dependencies.cmake` under `if(ENABLE_*)`. No directory globs.

**Path contract:**
| Style | Configure | Binary dir | When |
|-------|-----------|------------|------|
| Preset (preferred) | `cmake --preset linux-tests` | `out/build/linux-tests` | Daily tests / CI-shaped work |
| Daily desk | `cmake --preset linux-dev` | `out/build/linux-dev` | Shadow + ImGui + venues (also `./launch-desk.sh`) |
| Ad-hoc (avoid multi-tree) | `cmake -B build -DBUILD_TESTS=ON` | `build/` | One-off only; delete when done |

Matching build presets exist so `cmake --build --preset <name>` works with the preset `binaryDir`. Build presets default to one job; raise parallelism explicitly only after checking available RAM. `ctest --preset linux-tests` likewise runs serially with failure output enabled.

**Disk budget / cleanup:**
```bash
./scripts/clean-builds.sh                         # list sizes (dry-run)
./scripts/clean-builds.sh --keep linux-tests --apply
./scripts/clean-builds.sh --keep linux-tests --keep linux-dev --apply
./scripts/clean-builds.sh --stale 14 --apply      # drop trees idle ≥ 14 days
./scripts/clean-builds.sh --all --apply           # wipe every local build tree
```
Keep ≤ 1–2 warm trees. Drop ASAN/TSAN/bench trees after the session that needed them.

**Full-featured one-off** (prefer a named preset when possible):
```bash
cmake --preset linux-providers-questdb
cmake --build --preset linux-providers-questdb
ctest --test-dir out/build/linux-providers-questdb -j1
```

**CMake Presets** (recommended combinations):
```bash
cmake --list-presets
cmake --preset linux-tests && cmake --build --preset linux-tests
cmake --preset linux-research-only      # Release + tests; no engine_live target/artifact
cmake --preset linux-dev               # venues + ImGui + tests (daily desk)
cmake --preset linux-binance-questdb   # Binance + QuestDB + tests
cmake --preset linux-bitget            # Bitget UTA
cmake --preset linux-bitunix           # Bitunix MD/shadow
cmake --preset linux-venues            # Binance + Bitget + Bitunix
cmake --preset linux-providers-questdb # all venues + QuestDB
cmake --preset linux-web
cmake --preset linux-asan             # ASAN+UBSAN (+ Binance)
cmake --preset linux-ubsan            # UBSAN only
cmake --preset linux-shared-lib       # BUILD_SHARED_LIB=ON (+ tests)
cmake --preset linux-tsan
cmake --preset linux-benchmarks       # DEBUG + Google Benchmark
cmake --preset linux-release-native    # Release + NATIVE_OPT (all engines)
cmake --preset linux-release-low-memory # portable Release + tests, LTO off
```

**Key CMake Flags**:
- Venues: `ENABLE_BINANCE`, `ENABLE_BITGET`, `ENABLE_BITUNIX`.
- Feature: `ENABLE_QUESTDB`, `ENABLE_DEBUG` (Abseil), `ENABLE_BENCHMARKS`, `ENABLE_WEB` (civetweb — see [05-web-ui.md](05-web-ui.md)), `ENABLE_IMGUI` (GLFW/OpenGL desk).
- Compatibility only: `ENABLE_LIVE_DATA` is a deprecated no-op. Live market data is provided by the concrete venue options above.
- Build: `CMAKE_BUILD_TYPE=Release`, `ENABLE_LTO` (first-party Release targets; disable for lower peak memory), `ENABLE_NATIVE_OPT` (all three engines when ON), `BUILD_TESTS`, `BUILD_SHARED_LIB`.
- Distribution boundary: `TRUETEST_RESEARCH_ONLY=ON` omits the live target, exclusive live entry point, installation, and binary CPack artifact. Default `OFF` preserves compatibility. Research CPack is TGZ-only; `ENABLE_BINANCE` and `ENABLE_BITGET` are rejected until their market-data and live-order object graphs are separated.
- Sanitizers (Debug): `ENABLE_TSAN` is mutually exclusive with ASAN/UBSAN; `linux-asan` enables ASAN+UBSAN together, `linux-ubsan` enables UBSAN only.
- Perf reference build: Release + ENABLE_DEBUG + NATIVE_OPT + BENCHMARKS.

**Build audit header**: Every binary prints `AUDIT: git=... timestamp=... pins=...` (truetest_version.h generated).

**Presets, install, packaging**: `CMakePresets.json` is the source of truth for named presets. The root `CMakeLists.txt` contains the current `install()` and CPack TGZ/DEB rules; inspect those files before scripting a release package.

**FetchContent pins** (CLI11, zstd, nlohmann/json, etc.) live in the CMake dependency files. `ENABLE_DEBUG` links its instrumentation dependencies through `engine_core`, so those dependencies are present in every enabled target, including `engine_live`. The hot-path JSON allow-list remains CI-enforced.

**Performance build/instrumentation** ([../architecture/04-performance.md](../architecture/04-performance.md); deeper optimization notes planned — current details also in `AGENTS.md` preferred commands):
- Reference workload: 50k-bar synthetic CSV, SMA, inline preset (baseline ~36.67s wall, 1.4k ev/s on Ryzen 9 5900X; 5+ median runs).
- Instrumentation: StageTimer (9 stages: market_create, strategy, orderbook, fill, ring_publish, risk_check, mm_replenish, ...), ring_stats (drops/HWM critical, 0 drops required in prod), memory/copy trackers.
- Investigation: reproduce → read StageTimer/ring/copy → microbenchmark → update the measured baseline in [../architecture/04-performance.md](../architecture/04-performance.md) and add a regression guard.
- Historical wins (locked): mimalloc (tails -29%), PGO (layout/tails -47-97% on cheap stages), absl::flat_hash_map (maps), deque->ring + prealloc scratch (mm_replenish), etc. Dominant remaining cost: mm_replenish + market_create alloc volume (~1M make_shared<order> per 50k bars).

---

<a id="cli-reference"></a>
## 6. Complete CLI Flags, JSON Config, TUI, Dry-Run (instructions.md exhaustive)

**Run profiles and precedence**: `--preset` selects a named option bundle. Profile-owned values are applied after JSON configuration; explicit CLI flags take priority. There is no `TRUETEST_CONFIG` environment variable. Use `--dump-config` (snake_case JSON) to inspect the resolved options; `--dry-run` validates and exits 0/1 without running.

**Core groups** (selected critical; full tables in original instructions §12):
- Mode: `--mode backtest|shadow|live`, `--live` (required for real orders on mainnet + math captcha red banner; auto-skipped on --testnet).
- Provider: `--provider local|binance|binance-futures|bitget|bitget-futures|bitunix|bitunix-futures|synthetic`, `--symbol`, `--stream trade|kline_*|depth*`, `--depth-stream depth20@100ms` (for L2/queue), `--testnet`.
- Futures extras: `--margin-type isolated|cross`, `--margin-type-strict`, `--liquidation-warn-pct`, risk caps (`--max-notional`, `--max-leverage`, `--min-liq-distance-pct`), DMS (`--dead-man-countdown-ms 30000 --dead-man-heartbeat-ms 8000 --disarm-deadman`), kill (`--kill-switch-deadline-ms 5000`).
- Credentials: env `TRUETEST_BINANCE_*` (preferred; argv leaks to ps), `--api-key/--api-secret` (warns).
- Strategy: `--strategy sma,mean-reversion`, `--param key=value` (multi-strategy comma-separated).
- Risk/portfolio: `--balance`, `--risk-fraction`, platform `--sl`/`--tp` + `--exit-policy`, `--max-daily-loss`, `--max-trades-per-hour`, `--risk-unwind` (flag), `--reconcile-tolerance-bps`.
- Authoritative risk accounting (R3, see `docs/internal/r3-authoritative-risk-accounting.md`):
  - `--max-inventory-qty` — hard per-symbol inventory cap in base quantity. Applied to the **worst case**: current position + every open order on the increasing side + the candidate. Inventory-reducing orders stay permitted after a breach so inventory can always be flattened. `0` disables.
  - `--max-funding-8h-rate` — funding circuit breaker as a fraction of notional per 8 h (e.g. `0.0005`). Only enforced once a funding rate is actually known (derived from funding settlements); an unknown rate no longer reads as "inside the limit". `0` disables.
  - `--max-mark-age-ms` — age above which a mark counts as stale for risk. `0` disables the classification (backtest default; the sim clock jumps by whole bars).
  - `--risk-require-fresh-mark` — refuse inventory-increasing orders when the mark is stale or missing instead of falling back to the candidate's own limit price. Reductions stay allowed.
  - Position and portfolio exposure are mark-to-market (`|qty| * mark`); cost basis is P&L accounting only. Rejections carry a stable rule code (`max_open_orders`, `position_limit`, `hard_inventory_limit`, `portfolio_exposure`, `stale_mark`, `daily_loss`, `drawdown`, `funding_limit`, …) in the audit trail.
  - Removed: `max_portfolio_var_estimate`. It advertised a portfolio-VaR bound the engine never computed — see the R3 note §6.
- Realism (backtest/shadow only; bypassed in live): `--order-latency-us N --order-latency-stddev-us M`, `--impact-k-bps --impact-adv` (both required together), `--walked-book-impact`, `--fill-prob/--fill-fade/--fill-decay` (probabilistic limit-fill model, default off; fade applied pre-match), `--mm-levels/--mm-base-depth/--mm-spread-pct/--mm-vol-mult/--mm-max-spread-pct` (synthetic-book calibration), `--queue-model l2-snapshot` (shadow + depth-stream), `--maker-queue-model uniform|front|back` (paper/backtest; depth-stream optional — without L2, conservative size_ahead=+inf (no on_trade fills; bar/tick range sweep still fills passive limits); hybrid: limits→queue-aware, market/stop→local book so SL/TP never silent-drop). MC campaigns apply fee/latency/impact to every trial. Deprecated warn-noops: `--realistic-fills`, `--bar-spread-bps`.
- Threading: `--thread-preset inline|light|standard|full|extended` (auto from cores), `--spin-policy adaptive|spin|yield`, `--no-pin`, `--seed`.
- Profiles: `--preset futures-phase0|mc-robustness|backtest-local-l2|shadow-tape` (aliases are accepted; inspect the resolved configuration with `--dump-config`).
- Persistence: `--persist --run-tag myrun_YYYYMMDD_HHMM` (QuestDB), `--checkpoint path`.
- Replay/Record: `--replay events.bin --replay-from/--to`, `--record`, `--replay-data`.
- Output: `--output results.json`, `--status-format auto|tui|plain|ndjson|off`, `--no-tui`, `--simple-tui`, `--log-events`, `--log-file`, `--log-max-size`, `--log-keep`.
- Web UI (`-DENABLE_WEB=ON` only): `--web` (serve read-only web UI), `--web-port 8080`, `--web-bind 127.0.0.1`, `--web-token <tok>` (**required** for shadow/live; optional for backtest; also `?token=` in the browser), `--web-assets <dir>` (built SPA to serve at `/`). Streaming runs serve a live cockpit; backtest runs keep serving the final report until Ctrl-C. Read-only on every target — no order/flatten/kill routes. Full guide: [05-web-ui.md](05-web-ui.md).

**TUI**: Rich ncurses tabbed dashboard on shadow/live (positions, orders, L2, risk, brackets, debug StageTimer/ring, health/DMS counter). Hotkeys, setup menu on backtest. `--no-tui` for headless/CI.

**Web UI** (opt-in, `-DENABLE_WEB=ON` + `--web`): browser cockpit + backtest review, same data the TUI shows. Embedded civetweb server on its own thread, reading the engine through `snapshot_dashboard()` (off the hot path, outside the frozen live-safety surface). WS `/stream` (live SnapshotFrame), REST `/api/snapshot` + `/api/results`. See [web-ui.md](05-web-ui.md).

**JSON config**: Supported snake_case subset of CLI options, not a complete `engine_config` serialization. Use `--dump-config` to inspect the resolved output; consult `src/bin/main.inc` for the accepted keys.

**Launch wrappers**: only `launch-desk.sh` and `launch-test.sh` are supported convenience wrappers; see [LAUNCH_SCRIPTS.md](LAUNCH_SCRIPTS.md). Prefer the direct preset commands above for other runs.

---

<a id="providers-data-realism"></a>
## 7. Providers, Data Sources, Realism Models, Orderbook (consolidated)

**Providers** (`IProvider`; see also `docs/platforms/`):
- `local`: CSV OHLCV (bar) or tick-level; multi-path.
- `binance` / `binance-futures` (`ENABLE_BINANCE`): Combined trade + depth WS, REST execution (HybridExecutor paper/shadow, signed REST + user-data WS live), L2 seeding when `--depth-stream`. Candle backfill is currently **explicitly unsupported and fail-closed**: direct Binance `kline*` runs must pass `--backfill 0` because the global default is 500. A positive count terminates startup before the backfill REST request or public WebSocket open. Trade streams are unaffected. Disabling backfill is containment only; it does not certify candle-time or REST/WS-boundary semantics.
- `bitget` / `bitget-futures` (`ENABLE_BITGET`): UTA USDT-M futures; `--demo`/`--testnet` → paptrading; depth e.g. `books5`. Candle backfill is currently **explicitly unsupported and fail-closed**: direct Bitget `kline*`/`candle*` runs must pass `--backfill 0` because the global default is 500. This bypasses only the disabled backfill path and does not certify the still-open streaming time semantics. Bitget trade streams are unaffected. Ops: `docs/operations/03-bitget-demo.md`.
- `bitunix` / `bitunix-futures` (`ENABLE_BITUNIX`): MD + paper/shadow Phase 0–1; live order routing refused.
- `synthetic` / `montecarlo`: GBM paths (standalone or Monte Carlo).
- Replay: binary event log (`--replay`) or `--replay-data`.

**Data validation + formats**: Strict schema checks; see instructions §19.

**Realism models** ([../architecture/03-realism.md](../architecture/03-realism.md) planned — current summary: opt-in models default off, require `--depth-stream` for L2-dependent, **completely bypassed in live**; live venue supplies truth):
- **Fill pricing (always on, no flag)**: every fill records the resting counterparty's price, one fill_event per level walked. `market_aggression` (default 1.1) is purely a crossing guarantee — never a recorded price. The deprecated `--realistic-fills` / `--bar-spread-bps` are accepted as warn-noops.
- **Synthetic book calibration** (`--mm-levels` 10, `--mm-base-depth` 100, `--mm-spread-pct` 0.002, `--mm-vol-mult` 0.25, `--mm-max-spread-pct` 0.05): in bar mode the MarketMaker's seeded ladder is the sole source of spread cost for taker fills — calibrate it to the target market. The MM pulls and re-places its quotes each bar; resting strategy limits fill as maker orders when a quote update crosses their level, **or when the bar's [low, high] range trades through their level** (intrabar traversal sweep — fill at the order's own limit price).
- **Stop fills**: stops trigger on bar high/low and fill anchored at the stop price — or at the bar **open** when the bar gaps through the stop — never at the close. **ExitManager bracket fires use the same anchoring**.
- **Intra-bar ambiguity**: ExitManager resolves SL-vs-TP worst-case (SL first when both extremes cross in one bar), and tests trailing stops at their **pre-bar** level.
- **Bracket sizing across partial fills**: an opener that walks multiple book levels emits one fill per level; the armed bracket grows with each partial (entry reference = VWAP across opener fills).
- **Probabilistic limit fills** (`--fill-prob`, `--fill-fade`, `--fill-decay`; default off): RealisticFillModel gates each limit submit.
- Latency: two layers (`latency_model` strategy->eligible, `wire_latency_model` order->venue).
- Impact: SquareRootImpactModel raises the market-order reference before aggression — recorded prices always come from resting levels. `--walked-book-impact` uses the real L2 walked VWAP as reference when depth is present.
- Queue: `--queue-model l2-snapshot` (shadow L2SnapshotQueueModel for adverse-selection honesty), `--maker-queue-model uniform|front|back` (QueueAwareBookAdapter + IQueueModel for paper/backtest maker fills).

**Orderbook**: price-time priority matching; FillModel for partials; walked-book impact when L2 present.

**Futures vs Spot differences**: See the [Phase-0 futures SOP](../operations/01-futures-phase0-operator-sop.md), [futures testnet runbook](../operations/02-futures-testnet.md), and venue notes under `docs/platforms/` (one-way mode, non-atomic reduce-only brackets, funding, liquidation math, and position reconciliation).

---

## 8. Strategies, Exits, Brackets, Full Order Lifecycle

Strategies self-register via `REGISTER_STRATEGY` (sma, mean-reversion, ma-crossover, hedge-demo + indicators sma/ema/rsi/bollinger). Multi-strategy support. Emit `order_event` + `exit_intent` vectors.

**ExitManager + brackets**: Per-lot SL/TP/trailing; `IBracketAdapter` (OCO
spot, separate `closePosition` conditional algo orders on Binance futures).
Futures placement is non-atomic and does not assume venue sibling auto-cancel.

**Full live futures lifecycle** (see the Phase-0 SOP and venue notes linked above):
1. Strategy decision.
2. Pre-submit checks (RiskManager + venue FuturesRiskCheck + reconciler + one-way/symbol/WAF/clock).
3. ClientOrderId mint (tt-<epoch_hex>-... for idempotency/WAF-safe).
4. Encode/sign (no hot-path JSON).
5. REST POST /fapi/v1/order (or batch).
6. User-data WS (ORDER_TRADE_UPDATE + ACCOUNT_UPDATE) as truth.
7. Fill -> Portfolio (per-lot) + ExitManager brackets + Analytics + QuestDB + rings.
8. Bracket handling (two POSTs).
9. Shutdown/kill-switch (cancel-all + reduceOnly MARKET, hard deadline).
10. DMS (independent heartbeat + venue countdownCancelAll).

Order failure and catcher-layer details live in the Phase-0 SOP, testnet runbook, and venue-specific implementation notes.

---

## 9. Risk Management, DMS, Kill-Switch, Reconciler, WorkerWatchdog (detailed in docs/operations/02-futures-testnet.md, killswitch timeline, prod, user-manual)

**Layered**:
- Venue `IRiskCheck` / `FuturesRiskCheck` (notional, leverage, liq-distance, real tiered MMR) - hot path, before RiskManager.
- RiskManager (balance, daily loss, % equity/vol sizing, spread/funding CBs -> halt).
- Startup IReconciler (position/order vs /positionRisk + availableBalance; refusal default).
- Shutdown IKillSwitch (cancel-all + reduceOnly flatten, deadline).
- DMS (countdownCancelAll heartbeat; protects orders only; WorkerWatchdog monitors; 3× heartbeat internal).
- Halt propagation to all workers + ring policy `halt_on_drop` on safety rings.

**Futures testnet DMS validation playbook** (`docs/operations/02-futures-testnet.md` - 5 scenarios A-E with tables, conservative caps command, aliases `bf-orders`/`bf-position`, pass/fail, recording):
- A: Clean SIGINT.
- B: SIGKILL.
- C: OOM simulation.
- D: Network unplug (physical or iptables).
- E: SIGSTOP (foot-gun demo showing DMS still fires).

**Kill-switch LAN-unplug behavior**:
- The dedicated safety REST lane bounds DNS, connect, TLS, write, and read under one deadline and makes no retry.
- WS keepalive/Beast timeout -> fatal -> halt.
- DMS venue timer still fires and cancels orders (~30s).
- A disconnected kill attempt fails loudly within its deadline; no local path can guarantee a remote flatten without network reachability.
- The first failed DMS heartbeat latches terminal halt and routes the sole orderly flatten attempt through the shared exact-once kill session. A failed/ambiguous kill leaves the venue countdown armed as the independent order-cancel fallback.

**Refusal modes table** (clock skew, hedge, symbol not found, permissions, reconciler drift, etc.) in `docs/operations/02-futures-testnet.md`.

---

## 10. Threading Model, Workers, Observability (from instructions + engine-optimization + user-manual)

Lock-free SPSC RingBuffer (64k slots) per worker preset. Presets: inline (single-thread), light/standard/full/extended (progressive workers: Logging, Risk, Stats, Observer, MM replenishment).

**WorkerWatchdog**: Monitors heartbeats; triggers halt on stall.

**Observability**:
- StageTimer + ring_stats + memory/copy trackers (TUI debug tab + shutdown report).
- Structured logging (L1), rotation (L3), ndjson.
- Binary zstd event log (mandatory for replay/determinism).
- QuestDB (opt-in).
- Rich TUI panels (positions, L2, risk, brackets, DMS counter, health, debug).
- Analytics (Welford online, Sharpe/Sortino, adverse selection, report_generator).

**Ring drop policy**: `halt_on_drop` on safety-critical rings in shadow/live.

---

## 11. QuestDB Persistence (instructions §23 + db.md full authoritative)

**Build**: `-DENABLE_QUESTDB=ON` (raw POSIX sockets; zero new runtime deps).

**Runtime**: `--persist --run-tag myrun` (validated chars). Soft warning + continue (disabled) if daemon unreachable. Hard-fail via `--persist-strict` (implemented under `HAS_QUESTDB`; see `docs/reference/04-flags.md`).

**Current as-built implementation** (db.md explicit): Direct calls to `QuestdbStore` (mutex-protected) from engine capture points; the synchronous `IlpWriter` batches rows and flushes over TCP. Original ring + QuestDbWorker design was simplified and never built (historical text retained in db.md for audit).

**Schema** (full DDL in db.md Appendix A):
- `runs_meta` (two rows per run: sync + worker for dedup).
- 6 per-run tables: orders, fills, order_status, rejections, position_snapshots, funding_events (with opener_order_id, source, strategy_name, futures flags, signed qty, queue_position, etc.).
- PARTITION BY day, TIMESTAMP, SYMBOL, CAPACITY.

**ILP format** (Appendix B), health-check example, Docker recipe + readiness loop (Appendix C), example queries (GROUP BY run_tag, LAST, etc.) in instructions §23.

**Capture points**: engine process_order, fill loop, cancel, bracket, funding, position snapshot, etc. (see engine.cpp).

**Consumers**: dedup on order_id preferring sync row; two rows exist because of QuestDB partitioned UPDATE limitations.

---

## 12. Event Pipeline, Checkpoints, Analytics, Replay, Determinism

Full `provider::event` variant + market/tick/l2/order/fill/funding. OrderTracker lifecycle statuses. Cancel/amend/partial/TIF handling.

**Checkpoints**: Portfolio snapshots are diagnostic-only. `--resume` and direct `resume_checkpoint_path` are refused because v1 does not contain enough state for safe recovery. A future v2 must cover orders, lots, strategy, risk, and execution state. Backtest and Monte Carlo require an explicitly supplied deterministic `--seed`; the value `0` is valid only when explicitly present and never requests entropy.

**Deterministic manifest replay**: `--write-run-manifest`, `--replay-run-manifest`, `--artifacts-dir`, `--verify-hashes`, and replay-only `--trial` provide the versioned R6 run contract. Dataset/build identity, full effective model configuration, per-trial lifecycle artifacts and hash semantics are specified in [08-reproducibility.md](08-reproducibility.md).

**Replay**: `--replay events.bin` applies a current-v2 recorded economic ledger exactly once; it does not rerun a strategy or regenerate fills. Supply the same `--balance` as the recorded run. `--replay-from` and non-default `--replay-to` are refused: checkpoint prefix state is unavailable and record append order need not be monotonic in exchange-event time. Legacy/headerless logs remain available to `EventReplayer` for inspection but are not accepted as authoritative engine ledgers. `--replay-data` is the separate market-data path. Regression tests compare orders, fills, trades and PnL against the source run.

**Analytics**: Cumulative + rolling, per-symbol/strategy, alpha/beta vs benchmark, adverse selection, report export JSON/CSV.

---

## 13. Target Architecture, Migration History, Performance Baselines

**[../architecture/01-target-architecture.md](../architecture/01-target-architecture.md)** (north star, Phase 1 artifact):
- 6 guiding principles (compile-time safety, terminal halt, provider + 4 hooks, hot-path discipline, observability by default, small capital first).
- Steady-state components: event/execution model, layered risk/safety, persistence (binary mandatory, QuestDB opt-in), providers (local/binance + future drift/), observability (TUI/ndjson/Prometheus future).
- Deferred: hedge, COIN-M, generic cross-margin, web UI (removed).

**Change history**: use version control plus the dated records under `docs/decisions/`; there is no separate migration ledger.

**[../architecture/04-performance.md](../architecture/04-performance.md)**: Capacity and performance invariants, measurement requirements, and regression targets. Record reproducible before/after evidence with each optimization.

---

## 14. Strategy Validation Roadmap (strategy-validation.md)

Four ranked future deliverables (mostly planned):
1. Dual-portfolio shadow (second Portfolio in engine_shadow consuming exchange_filled; sim vs exch equity/PnL/Sharpe/DD at shutdown; QuestDB suffixed; highest leverage).
2. A/B comparison CLI (Python + QuestDB queries, side-by-side + curves + bootstrap CI).
3. Per-trade analyzer (round-trips, MAE/MFE, slippage vs price series).
4. L2-aware queue position modeling in TradeTapeShadowAdapter + schema column.

Acceptance commands and methodology in the doc. Ties to realism + demo-trading-workflow.

---

## 15. Operator SOPs, Testnet Guides, Demo Workflows, Killswitch Timeline

Full operational details, 5-step validation ladder, DMS playbook, testnet guides, order lifecycle, killswitch timeline live in [../governance/01-prod.md](../governance/01-prod.md), reports/phase0/, and planned docs/operations/ (see aspirational note in docs/README.md). Current ritual + templates in 01-prod.md + reports.

**Key files for technical reference** (when present):
- [../operations/01-futures-phase0-operator-sop.md](../operations/01-futures-phase0-operator-sop.md) and [../operations/02-futures-testnet.md](../operations/02-futures-testnet.md) (operator lifecycle and non-atomic bracket caveats)
- killswitch-lan-unplug-timeline.md (network partition analysis)
- User data WS as source of truth, DMS orders-only.

**Note**: Aspirational SOPs use "Planned for Doc Phase X – current details live in docs/governance/01-prod.md". See reports/phase0/ for current evidence machinery.

---

## 16. Phase 0 Evidence Collection

Full details, templates, PROGRESS tracker, ritual live in `reports/phase0/` + [../governance/01-prod.md](../governance/01-prod.md). This section points only.

Use reports/phase0/PROGRESS.md (0/15), templates/, scripts/phase0/ for collection. See 01-prod.md for exit criteria.

---

## 17. C API & Embedding

Current surface (opaque handle; `tt_create_engine` accepts a C-API-specific
schema that maps selected fields into `engine_config`):

- `tt_version()`, `tt_create_engine(config_json)`, `tt_run(handle)`,
  `tt_get_results()` (JSON, caller frees), `tt_last_error()`,
  `tt_free_string()`, `tt_destroy()`; declarations live in
  [`src/api/truetest_api.h`](../../src/api/truetest_api.h).
- Required JSON field: string `data_path`. Optional fields currently parsed:
  `symbol`, `strategy`, `initial_balance`, `seed`, `rolling_window`,
  `risk_free_rate`, `periods_per_year`, `market_aggression`, `fill_rng_seed`,
  `event_log_path`, and numeric strategy `params`.
- The root must be an object. Unknown fields, wrong field types, non-finite
  numbers, duplicate object keys at any depth, an empty `data_path` or strategy
  name, and nonnumeric strategy params are rejected at create time.
  `initial_balance`, `rolling_window`, and `periods_per_year` must be positive;
  `market_aggression` must be in `(1, 2)`. Seeds are unsigned integers.
  `fill_rng_seed` controls the local fill RNG only when `seed` is omitted or
  zero; supplying both a nonzero `seed` and `fill_rng_seed` is rejected rather
  than silently ignoring one value.
- `symbol` is an optional authoritative dataset identity, for example
  `{"data_path":"bars.csv","strategy":"sma","symbol":"BTCUSDT"}`. It must
  be nonempty and contain no ASCII whitespace/control bytes. Matching is exact;
  the C API does not trim or canonicalize case.
- If any loaded bar or tick has no symbol, `symbol` is required. When supplied,
  every existing nonempty identity is checked first; only then are blank
  records bound. A mismatch fails closed without relabeling the dataset.
- C API batch loads are strict: if any input row is malformed or rejected by
  domain validation, the complete loaded batch is rolled back and `tt_run`
  returns code `3`; the diagnostic includes the rejected-row count.
- Fully named input needs no synthetic identity; when `symbol` is omitted, the
  identity policy preserves existing names. This ingestion rule does **not**
  establish correctness of multi-symbol benchmark/return analytics (C-15 is
  still open).
- `tt_run` executes a handle at most once. Concurrent or later calls return the
  first attempt's cached code/error without reloading data or running the engine
  again. `tt_get_results()` is available only when that attempt succeeded;
  inspect `tt_last_error()` after any nonzero result.
- `tt_last_error()` is thread-local and its pointer remains valid only until the
  next C API call on that same thread. Each non-null `tt_get_results()` buffer is
  separately allocated and must be passed to `tt_free_string()`.
- Handle lifetime is caller-owned: `tt_destroy()` is valid only after every
  in-flight `tt_run()`/`tt_get_results()` call using that handle has returned
  and been joined. Concurrent destroy/use is unsupported.

| Code | Meaning |
|------|---------|
| `0` | Engine run completed successfully |
| `1` | Null handle |
| `2` | Unknown strategy |
| `3` | Data load or dataset-symbol-policy failure |
| `4` | Exception while preparing or running the engine |
| `5` | Engine returned from `run()` halted or failed |

- In the C API, nlohmann/json is used only at the API boundary, never on the
  hot path.
- **Limitations today**: local batch backtesting only. The C API does not expose
  provider, streaming, replay, shadow, live-order, TUI, or QuestDB paths.
  Custom `qty_scale` is rejected until quantity-to-atom conversion failures can
  propagate transactionally through order submission; `spread_step_factor` is
  rejected because it has no consumer on this local path.

---

## 18. Licenses & Third-Party (planned planned licenses reference (not yet written); current pins in AGENTS.md + build files)

Authoritative table:
- Always: CLI11 (BSD), zstd (BSD chosen over GPL), nlohmann/json (MIT).
- Conditional: Boost (BSL for enabled venues), OpenSSL (Apache for enabled venues), Abseil (Apache for DEBUG - never engine_live), GTest/Benchmark (tests only).
- QuestDB: zero new deps (raw POSIX).
- Rules: No vendored trees, copyleft forbidden (dual-license election documented), update table + pins in same PR, engine_live column, hot-path JSON CI gate.

---

## 19. Future Directions & Upcoming (upcoming/*.md + prod phases + target)

**Drift Protocol Liquidation Keeper Bot** (claude_drift.md + grokd_drift.md - two detailed overlapping plans, May 2026):
- Non-speculative infrastructure play (inventory risk on unwind acknowledged; small capital limits scope).
- Engine reuse ~25-30% (IProvider + 4 safety hooks, rings, WorkerWatchdog, DMS/Kill/Reconciler patterns, event log/replay, QuestDB/TUI/config, Boost.Beast/OpenSSL). Do not reuse orderbook/strategies/Binance specifics.
- Recommended: Hybrid C++ engine + Rust FFI (`drift-rs`) for decode/margin/tx build (Solana not natural in C++).
- 7-phase plan (both docs): Phase 0 skeleton (ENABLE_DRIFT + keeper_sim/keeper_live targets + run_keeper() + FFI stub), 1 (multi-RPC + simulateTransaction mandatory), 2 (UserMap + exact margin health via FFI - hardest), 3 (profitability sim post-unwind), 4 (tx build v0+ALT + Jito + unwind queue), 5 (capital caps + adapted safety + external supervisor mandatory vs SIGSTOP), 6 (systemd + alerting), 7 (replay/shadow -> tiny rollout with 70-80% shadow success gate).
- Cross-cutting: simulation gate non-negotiable, resubscribe-on-reconnect, dedicated RPCs, external supervisor, exact on-chain math.
- Bottom line: Shadow run (economic model) is make-or-break before any capital.

Phases 3-6 roadmap: see [../governance/01-prod.md](../governance/01-prod.md). Other deferred items in target-architecture (planned).
---

## 20. Consolidated Checklists & Procedures

Full authoritative checklists, Phase 0 ritual, Phase 1 edit procedure, Go-Live Gate, DMS validation scenarios live in [../governance/01-prod.md](../governance/01-prod.md), [../governance/02-prerequisites.md](../governance/02-prerequisites.md), reports/phase0/, and AGENTS.md.

**Technical pointers kept here**:
- Pre-merge: run freeze script, use correct model, no anti-patterns.
- Perf: 5+ median runs + update baseline.
- QuestDB health: soft warning in default mode; any startup/runtime failure is
  terminal and nonzero when `--persist-strict` is selected.
- DMS / network drills: see killswitch timeline refs.

See governance for full "Go-Live Gate: all 9 rows...".

---

<a id="critical-warnings"></a>
## 21. Master List of Critical Warnings & Non-Negotiables

Full governance warnings in [../governance/01-prod.md](../governance/01-prod.md) + AGENTS.md. Key technical non-negotiables (preserved):

- Halt is terminal - no resume/retry on safety paths.
- No JSON on hot path; SPSC discipline; no second producer.
- DMS **does not close positions** (orders only).
- SIGSTOP defeats in-process kill; DMS orders-only fires.
- Blocking connect foot-gun on network loss.
- One-way mode hard refusal; reconciler default-refuse.
- Math-captcha + attended for mainnet live.
- Testnet: no realism calibration.
- Post-halt: mandatory grep before resume.
- Realism/queue/impact **bypassed in live**.
- Ring drops on safety -> halt_on_drop.
- Agent cannot decide live capital.

See 01-prod.md for capital tier / Go-Live rules.

---

## 22. Source Code Cross-References (key locations from all docs)

- `src/core/tt_target.h` (TT_TARGET, target_allows_live_orders, frozen).
- `src/engine/engine.{h,cpp}` (hot loop, process_order ~1600-1628 risk, LocalBookAdapter ctor ~203, l2_seeded ~1721, on_exchange_fill ~2343, log_event sites, build_dashboard_view, publish, restore_state, pending_orders_, mm_replenish, StageTimer sites, dual-portfolio wiring, shutdown, halt propagation).
- `src/providers/binance/` (32 files): binance_futures_provider.h (open ~224-410, position_snapshot ~366-375, funding log ~647-655, leverageBracket, one-way probe, advisories), dead_mans_switch.h (class comment ~31-33), kill_switch.h, reconciler.h, safety.h, bracket_adapter, user_data_parser (funding reason), rest_client (blocking connect), combined parser, ExecutionBridge.
- `src/risk/` (risk_manager, futures_risk_check ~154-176 approx, maintenance_margin_table).
- `src/execution/` (portfolio (lots_, on_funding), execution_adapter (LocalBookAdapter, QueueAware), latency/impact/queue_model/queue_position_model, trade_tape_shadow_adapter, live_safety, order_tracker, fee_model, etc.).
- `src/orderbook/`, `src/exits/` (ExitManager, bracket_adapter), `src/strategy/` (registry), `src/analytics/` (shadow_tracker, adverse_selection, on_funding).
- `src/data/questdb/` (store, ilp_writer, schema, http/tcp_client, run_tag - current direct mutex impl).
- `src/threading/` (ring_buffer, worker_watchdog, thread_preset, worker).
- `src/debug/` (stage_timer, ring_stats, memory_info, copy_tracker).
- `src/core/event.h` (funding_event), `src/bin/main.inc` (CLI wiring, defaults DMS 30s/10s, kill 5s).
- `src/api/truetest_api.{h,cpp}`.
- `CMakeLists.txt`, `cmake/{Dependencies,CompilerFlags}.cmake`, `scripts/{check-live-safety-freeze.sh, check-hotpath-json.sh, phase0/*, pre-commit}`.
- `tests/` (golden_regression, binance_futures_testnet_live, questdb_*, impact/latency/queue, etc.).

Cross-references point to files now organized under `architecture/`, `operations/`, `reference/`, plus the governance under `docs/governance/` and `reports/phase0/`. See [../README.md](../README.md) for the current layout.

---

## 23. Document Maintenance & Evolution

- Living documents — update after phase completion, before capital tier increase, after material frozen surface change.
- Every PR touching core/risk/safety must reference [../governance/03-todo.md](../governance/03-todo.md) items (high-level) or a specific todo such as `docs/todos/02-P1-freeze.md` P1-02 and run the freeze script.
- This instructions.md contains pointers + technical how-to (CLI, providers, build, flags, MC, QuestDB, realism, diagrams). Long-form phase/ritual/gates in [../governance/01-prod.md](../governance/01-prod.md).
- Phase 0/1 artifacts permanent in reports/phase0/.

**Related authoritative files**: [../../AGENTS.md](../../AGENTS.md), [../governance/01-prod.md](../governance/01-prod.md), [../governance/02-prerequisites.md](../governance/02-prerequisites.md), [../governance/03-todo.md](../governance/03-todo.md), [../governance/04-summary.md](../governance/04-summary.md) + `docs/reference/` + `docs/README.md`. Historical in `docs/archive/`. Phase 0 evidence in `../../reports/phase0/`.

Aspirational refs use explicit "Planned for Doc Phase X – current in docs/governance/01-prod.md".

**docs/ is now the central authoritative documentation home.**

---

**End of Master Consolidated Instructions.** (Slimmed 2026-07 for duplication reduction.)

*Last updated: 2026-07 (docs overhaul)*
