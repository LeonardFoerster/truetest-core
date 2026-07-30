# TrueTest / hft-engine - Master Consolidated Instructions

**Status**: Single authoritative reference. Produced via multi-agent exhaustive extended-thinking analysis of every Markdown file in the repository. This document lives alongside a reorganized documentation set (see [docs/README.md](README.md) for the current structure). It supersedes the scattered prior documents while preserving all key substance.

**Version**: 1.0 (Consolidated 2026-05 from 4 parallel deep-dive subagent reports + source cross-checks)  
**Philosophy**: "Someone reading only one phase still has the context of the invariants." Deliberately comprehensive and repetitive on safety. Update after every phase or material change.

**Quick Links (Internal)**: [Philosophy & Invariants](#1-philosophy-invariants-safety-model) • [Production Phases](#3-production-readiness-phases) • [Build](#5-build-cmake-reference) • [CLI Reference](#6-complete-cli-flags-json-config) • [MC / Providers / QuestDB usage](#7-providers-data-sources-realism-models-orderbook) • [Critical Warnings](#21-master-list-of-critical-warnings-non-negotiables)

---

## 1. Philosophy, Invariants, Safety Model

TrueTest is a modular C++23 engine for reproducible backtesting, divergence-aware shadow trading, and gated live execution from a **single source tree**. Three binaries (`engine_backtest`, `engine_shadow`, `engine_live`) differ only by the compile-time `TT_TARGET` define in `src/core/tt_target.h`. Live-order paths are physically removed via dead-code elimination in non-live targets (`target_allows_live_orders()` is constexpr false).

**Core non-negotiable invariants** (full authoritative list + rationale in [../governance/01-prod.md](../governance/01-prod.md); also CLAUDE.md):

1. **Compile-time live-order gate is absolute** - Only `engine_live` (and future keeper_live targets) can ever emit real orders/transactions. Never introduce runtime `allow_live_orders` checks or recompile-time bypasses.
2. **Halt is terminal** (`halt_flag_` in engine/risk) - Write-once atomic true; only manual operator intervention + explicit process restart clears it. No auto-resume, no SIGUSR1, no cooldown, no "helpful" retry logic on safety paths.
3. **Safety paths are loud and non-retrying** - Kill-switch, DMS, reconciler, WorkerWatchdog failures escalate to operator with clear diagnostics. No silent backoff/retry on safety surfaces.
4. **Hot-path discipline** (enforced by `scripts/check-hotpath-json.sh` + layer-deps) - Zero `nlohmann::json` on hot path; zero (or object-pool) allocations on event loop; lock-free SPSC RingBuffer (65536 slots, exactly one producer/consumer); no second writer on any ring.
5. **Reconciler refusal is default** - Blocks startup on position/order drift > tolerance (configurable `--reconcile-tolerance-bps`). Only documented soft-warn exception: spot testnet monthly account resets (futures has none).
6. **User-data WebSocket is source of truth** (Binance futures/spot) - `ORDER_TRADE_UPDATE` + `ACCOUNT_UPDATE` after initial REST ack. Position snapshots from REST are advisory only until reconciled.
7. **DMS protects orders only** (`/fapi/v1/countdownCancelAll`) - Venue-side auto-cancel on heartbeat loss. Does **not** emit reduceOnly MARKET flattens (Phase 3 work). Kill-switch (orderly) does cancel-all + reduceOnly flatten with hard deadline.
8. **Futures mandates** - One-way mode hard refusal in `BinanceFuturesProvider::open()`; `reduceOnly` + `closePosition=true` brackets (non-atomic, two POSTs with auto-cancel guarantee); pre-trade venue `FuturesRiskCheck` (notional/leverage/liq-distance + real tiered MMR from `/fapi/v1/leverageBracket`) consulted **before** `RiskManager` in hot path (`engine.cpp:1600-1628`).
9. **Provider abstraction is the sole extension point** - `IProvider` + four safety hooks (`IReconciler`, `IKillSwitch`, `IRiskCheck`, `IBracketAdapter`) + transport/parser/executor. Core engine never contains `#ifdef HAS_*` or venue specifics.
10. **Small capital first + evidence-based gates** - Every phase exit requires artifacts (binary logs, QuestDB run_tag, signed notes, shadow reports) + two-person sign-off before capital tier increase. "No capital tier increase is permitted until all nine rows [Go-Live Gate] have two signatures."

**Additional strong primitives already present**: Layered risk (venue first), `WorkerWatchdog` (3× heartbeat), clock-skew/WAF/symbol existence/one-way probes at open, `ExecutionBridge` mutex audit, rate limiter, per-lot `Portfolio` + `ExitManager`, binary zstd event log + replay, QuestDB (soft-fail today), rich ncurses TUI, StageTimer/ring stats observability.

**Philosophy quote** ([../governance/01-prod.md](../governance/01-prod.md)): "The engine already implements strong compile-time and runtime safety primitives that most retail or early-stage systems lack... This playbook exists so that future operators cannot say 'we forgot why we were careful.'"

---

## 2. AI Coding Assistant Rules & Model Selection + Phase 1 Live-Safety Freeze

From CLAUDE.md + [architecture/MODEL.md](architecture/MODEL.md) (full rationale):

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
- **Anti-patterns** ([architecture/MODEL.md](architecture/MODEL.md) explicit rejects): retry on kill-switch, resettable halt, hot-path JSON, runtime live-order check, HAS_* in core, second producer on SPSC, reconciler soft-warn for convenience, adaptive heartbeat.

**Phase 1 Live-Safety Freeze**: See full in [../governance/01-prod.md](../governance/01-prod.md) and [../governance/02-prerequisites.md](../governance/02-prerequisites.md). 10 files under `LIVE_SAFETY_CCB_APPROVED` token + CCB + clean multi-hour `engine_shadow` (enforced by scripts/check-live-safety-freeze.sh). 

**Pre-merge safety checklist** (see CLAUDE.md + prod): model used, target gate not bypassed, halt terminal, scripts pass, no anti-patterns.

---

## 3. Production Readiness Playbook & Capital-Tier Phases

Full authoritative details, Phase 0/1 gates, exact ritual, Go-Live table (9 rows requiring two signatures + evidence), and philosophy live in [../governance/01-prod.md](../governance/01-prod.md) (and `reports/phase0/` for evidence). This file focuses on technical how-to and pointers.

**Key Phase 0 command template** (see 01-prod.md for full "why" + exit criteria + ritual):
```bash
./build/engine_live \
  --provider binance-futures \
  --symbol BTCUSDT \
  --stream trade --depth-stream depth20@100ms \
  --live \
  --api-key "${BINANCE_FUTURES_KEY}" --api-secret "${BINANCE_FUTURES_SECRET}" \
  --persist --run-tag p0_$(date +%Y%m%d_%H%M) \
  --reconcile-tolerance-bps 3 \
  --dead-man-countdown-ms 30000 --dead-man-heartbeat-ms 8000 \
  --max-notional 15000 --max-leverage 2.5 --min-liq-distance-pct 7 \
  --max-daily-loss 80 --risk-unwind 0.4
```
Status: 0/15 qualifying (see reports/phase0/PROGRESS.md + docs/governance/03-todo.md).

**Phase 1 Live-Safety Freeze**: frozen safety files (Binance + Bitget futures + core/engine/risk/threading; see `scripts/check-live-safety-freeze.sh`) + `LIVE_SAFETY_CCB_APPROVED` token + CCB + clean shadow run enforced (see docs/governance/01-prod.md, 02-prerequisites.md).

**Phases 2–6**: High-level roadmap in docs/governance/01-prod.md.

**Go-Live Gate**: See full 9-row table in docs/governance/01-prod.md. No capital tier increase without all + two signatures + evidence.

---

## 4. Prerequisites, Change Control, Task Tracking

See [../governance/02-prerequisites.md](../governance/02-prerequisites.md) for the living Phase 1+ checklist (must be green before PRs touching frozen surface). Run `./scripts/check-live-safety-freeze.sh --check-head`, reference relevant items in [../governance/03-todo.md](../governance/03-todo.md) (or docs/todos/ e.g. docs/todos/02-P1-freeze.md#P1-02). Historical plans/gaps live in `docs/archive/`.

(Planned architecture docs under ../architecture/ use "Planned for Doc Phase X – current details in docs/governance/01-prod.md + this file".)

**Note**: docs/ is now the central authoritative documentation home. Governance files live under docs/governance/. Last updated: 2026-07 (docs overhaul).

See [../governance/03-todo.md](../governance/03-todo.md) (thin high-level) for the phased task list or precise docs/todos/01-P0-phase0.md#P0-01 etc (see docs/todos/00-OVERVIEW.md). Every frozen PR must reference relevant items (Addresses todo.md #P1-02 style preserved for backward).

reports/phase0/ contains the evidence machinery (README for layout, PROGRESS.md tracker, ops/ for batch reviews, templates/ for session notes). Current Phase 0 gates live in docs/governance/01-prod.md and docs/todos/01-P0-phase0.md.

---

## 5. Build & CMake Reference (from instructions.md + CLAUDE + perf docs)

**Minimal (zero external deps)**:
```bash
cmake -B build
cmake --build build
```
Produces `engine_backtest` (default), `engine_shadow`, `engine_live` (TT_TARGET=1/2/3).

Core and test source lists live in `cmake/Sources.cmake` (the single obvious place to register a new strategy, simulation component, or test).

**Full-featured** (or use presets for common combos):
```bash
cmake -B build \
  -DENABLE_BINANCE=ON \
  -DENABLE_BITGET=ON \
  -DENABLE_QUESTDB=ON \
  -DENABLE_LIVE_DATA=ON \
  -DENABLE_DEBUG=ON \
  -DENABLE_NATIVE_OPT=ON \
  -DBUILD_TESTS=ON \
  -DENABLE_BENCHMARKS=ON \
  -DBUILD_SHARED_LIB=ON
cmake --build build -j$(nproc)
ctest --test-dir build
```

**CMake Presets** (recommended for real combinations). Configure + build share the
same preset name; trees live under `out/build/<preset>` (not the ad-hoc `build/` dir):
```bash
cmake --preset linux-tests && cmake --build --preset linux-tests -j
cmake --preset linux-binance-questdb
cmake --preset linux-bitget
cmake --preset linux-providers-questdb   # Binance + Bitget + QuestDB
cmake --preset linux-web
cmake --preset linux-asan
cmake --preset linux-tsan
cmake --preset linux-benchmarks
cmake --preset linux-release-native
ctest --test-dir out/build/linux-tests
```

**Key CMake Flags** (see instructions.md §3 for exhaustive table):
- Feature: ENABLE_BINANCE, ENABLE_BITGET (UTA futures), ENABLE_QUESTDB, ENABLE_LIVE_DATA, ENABLE_DEBUG (Abseil), ENABLE_BENCHMARKS, ENABLE_WEB (embedded web UI server, fetches civetweb — see [web-ui.md](web-ui.md)).
- Build: CMAKE_BUILD_TYPE=Release (with DEBUG for instrumentation), ENABLE_NATIVE_OPT (all three engines when ON), BUILD_TESTS/SHARED_LIB.
- Sanitizers (Debug, mutually exclusive where noted): ENABLE_TSAN (preferred for threading), ASAN+UBSAN combos (OPTIONS="halt_on_error=1,abort_on_error=1,...").
- Perf reference build: Release + ENABLE_DEBUG + NATIVE_OPT + BENCHMARKS.
- Source registration: always-on core + tests in `cmake/Sources.cmake`; optional backends in `cmake/Dependencies.cmake` via `tt_wire_optional_backends`.

**Build audit header**: Every binary prints `AUDIT: git=... timestamp=... pins=...` (truetest_version.h generated).

**Presets, Install, Packaging**: See instructions.md §8-10 (linux-default preset, CPack TGZ/DEB).

**FetchContent pins** (CLI11, zstd, nlohmann/json, etc.) and license rules in [reference/licenses.md](reference/licenses.md) (no copyleft, Abseil never in engine_live, hot-path JSON CI gate).

**Performance build/instrumentation** ([../architecture/engine-optimization.md](../architecture/engine-optimization.md) + [../architecture/performance.md](../architecture/performance.md) + [../architecture/perf-baseline.md](../architecture/perf-baseline.md)):
- Reference workload: 50k-bar synthetic CSV, SMA, inline preset (baseline ~36.67s wall, 1.4k ev/s on Ryzen 9 5900X; 5+ median runs).
- Instrumentation: StageTimer (9 stages: market_create, strategy, orderbook, fill, ring_publish, risk_check, mm_replenish, ...), ring_stats (drops/HWM critical, 0 drops required in prod), memory/copy trackers.
- Investigation: reproduce -> read StageTimer/ring/copy -> microbench -> lock with baseline update in [../architecture/perf-baseline.md](../architecture/perf-baseline.md) + regression guard.
- Historical wins (locked): mimalloc (tails -29%), PGO (layout/tails -47-97% on cheap stages), absl::flat_hash_map (maps), deque->ring + prealloc scratch (mm_replenish), etc. Dominant remaining cost: mm_replenish + market_create alloc volume (~1M make_shared<order> per 50k bars).

---

## 6. Complete CLI Flags, JSON Config, TUI, Dry-Run (instructions.md exhaustive)

**Precedence**: CLI > JSON config (`--config path` or `TRUETEST_CONFIG`) > defaults. `--dump-config` (snake_case JSON), `--dry-run` (validate + exit 0/1 without running).

**Core groups** (selected critical; full tables in original instructions §12):
- Mode: `--mode backtest|shadow|live`, `--live` (required for real orders on mainnet + math captcha red banner; auto-skipped on --testnet).
- Provider: `--provider local|binance|binance-futures`, `--symbol`, `--stream trade|kline_*|depth*`, `--depth-stream depth20@100ms` (for L2/queue), `--testnet`.
- Futures extras: `--margin-type isolated|cross`, `--margin-type-strict`, `--liquidation-warn-pct`, risk caps (`--max-notional`, `--max-leverage`, `--min-liq-distance-pct`), DMS (`--dead-man-countdown-ms 30000 --dead-man-heartbeat-ms 8000 --disarm-deadman`), kill (`--kill-switch-deadline-ms 5000`).
- Credentials: env `TRUETEST_BINANCE_*` (preferred; argv leaks to ps), `--api-key/--api-secret` (warns).
- Strategy: `--strategy sma,mean-reversion`, `--param key=value` (multi-strategy comma-separated).
- Risk/portfolio: `--initial-cash`, `--risk-fraction`, `--sl-atr`, `--tp-atr`, `--max-daily-loss`, `--max-trades-per-hour`, `--risk-unwind 0.4`, `--reconcile-tolerance-bps`.
- Realism (backtest/shadow only; bypassed in live): `--realistic-fills`, `--order-latency-us N --order-latency-stddev-us M`, `--impact-k-bps`, `--bar-spread-bps`, `--queue-model l2-snapshot` (shadow + depth-stream), `--maker-queue-model uniform|front|back` (paper + depth-stream; uniform recommended default).
- Threading: `--thread-preset inline|light|standard|full|extended` (auto from cores), `--spin-policy adaptive|spin|yield`, `--no-pin`, `--seed`.
- Persistence: `--persist --run-tag myrun_YYYYMMDD_HHMM` (QuestDB), `--checkpoint path`.
- Replay/Record: `--replay events.bin --replay-from/--to`, `--record`, `--replay-data`.
- Output: `--output results.json`, `--status-format tui|ndjson|minimal`, `--log-events`, `--log-rotation`.
- Web UI (`-DENABLE_WEB=ON` only): `--web` (serve read-only web UI), `--web-port 8080`, `--web-bind 127.0.0.1`, `--web-token <tok>` (optional bearer auth; also `?token=` in the browser), `--web-assets <dir>` (built SPA to serve at `/`). Streaming runs serve a live cockpit; backtest runs keep serving the final report until Ctrl-C. Read-only on every target — no order/flatten/kill routes. Full guide: [web-ui.md](web-ui.md).

**TUI**: Rich ncurses tabbed dashboard on shadow/live (positions, orders, L2, risk, brackets, debug StageTimer/ring, health/DMS counter). Hotkeys, setup menu on backtest. `--no-tui` for headless/CI.

**Web UI** (opt-in, `-DENABLE_WEB=ON` + `--web`): browser cockpit + backtest review, same data the TUI shows. Embedded civetweb server on its own thread, reading the engine through `snapshot_dashboard()` (off the hot path, outside the frozen live-safety surface). WS `/stream` (live SnapshotFrame), REST `/api/snapshot` + `/api/results`. See [web-ui.md](web-ui.md).

**JSON config**: Full engine_config schema (mode, provider, strategy, risk, threading, persistence, realism, etc.). See instructions §13 for keys.

**start.sh launcher** and many numbered examples in instructions §29-35 (backtest minimal -> full futures live with DMS/persist/risk caps -> sanitizers -> PGO training -> replay -> QuestDB queries, etc.).

---

## 7. Providers, Data Sources, Realism Models, Orderbook (consolidated)

**Providers** (`IProvider`):
- `local`: CSV OHLCV (bar) or tick-level; BinaryCache decorator; multi-path.
- `binance` / `binance-futures`: Combined trade + depth WS, REST execution (HybridExecutor paper/shadow, signed REST + user-data WS live), L2 seeding for realism when `--depth-stream`.
- Replay: binary event log or `--replay-data`.
- Future: `drift` (see upcoming plans).

**Data validation + formats**: Strict schema checks; see instructions §19.

**Realism models** ([../architecture/realism.md](../architecture/realism.md) - all default off, require `--depth-stream` for L2-dependent, **completely bypassed in live**; live venue supplies truth):
- `--realistic-fills`: passive/resting prices, one fill_event per level walked.
- Latency: two layers (`latency_model` strategy->eligible, `wire_latency_model` order->venue).
- Impact: SquareRootImpactModel applied before aggression.
- Bar-spread: full bid-ask on bar-mode market orders (suppressed on L2 symbols).
- Queue: `--queue-model l2-snapshot` (shadow L2SnapshotQueueModel for adverse-selection honesty), `--maker-queue-model uniform|front|back` (QueueAwareBookAdapter + IQueueModel for paper/backtest maker fills; tracks size_ahead; real prints consume front; L2 shrinkage = cancels per model).

**Orderbook**: price-time priority matching; FillModel for partials; walked-book impact when L2 present.

**Futures vs Spot differences**: See futures-order-lifecycle.md table (one-way, reduceOnly brackets non-atomic, funding, liquidation math, position recon from /positionRisk, etc.).

---

## 8. Strategies, Exits, Brackets, Full Order Lifecycle (futures-order-lifecycle.md + exits/)

Strategies self-register via `REGISTER_STRATEGY` (sma, mean-reversion, ma-crossover, hedge-demo + indicators sma/ema/rsi/bollinger). Multi-strategy support. Emit `order_event` + `exit_intent` vectors.

**ExitManager + brackets**: Per-lot SL/TP/trailing; `IBracketAdapter` (OCO spot, separate reduceOnly STOPs + closePosition futures). Non-atomic on futures but with auto-cancel guarantee in adapter.

**Full live futures 10-step lifecycle** (futures-order-lifecycle.md):
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

**Where orders can fail** table (with catcher layer) in futures-order-lifecycle.md.

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

**Killswitch LAN-unplug timeline** (killswitch-lan-unplug-timeline.md - canonical catastrophic disconnect analysis):
- Blocking `net::connect()` in REST client (tcp_syn_retries=6 -> 60-90s hang) is a known foot-gun.
- WS keepalive/Beast timeout -> fatal -> halt.
- DMS venue timer still fires and cancels orders (~30s).
- Kill-switch and DMS flatten require the lost network -> wedge.
- Implications: unplug validates DMS + halt story, not flatten (use `--dms-attempt-position-close` + independent machine for drill). Phase 3 external watchdog + non-blocking connect needed.

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

**Runtime**: `--persist --run-tag myrun` (validated chars). Soft warning + continue (disabled) if daemon unreachable. Hard-fail (`--persist-strict`) is Phase 4 TODO.

**Current as-built implementation** (db.md explicit): Direct calls to `QuestdbStore` (mutex-protected) from engine capture points; batched `IlpWriter` on own thread. Original ring + QuestDbWorker design was simplified and never built (historical text retained in db.md for audit).

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

**Checkpoints**: Portfolio snapshots (netted positions today; lots_ richer in Phase 4). `--seed` for RNG + fixed epoch determinism.

**Replay**: `--replay events.bin` (or `--replay-data`), time-seeking `--replay-from/--to`, paced vs fast-forward. Golden regression tests for bit-identical fills/PnL.

**Analytics**: Cumulative + rolling, per-symbol/strategy, alpha/beta vs benchmark, adverse selection, report export JSON/CSV.

---

## 13. Target Architecture, Migration History, Performance Baselines

**[../architecture/target-architecture.md](../architecture/target-architecture.md)** (north star, Phase 1 artifact):
- 6 guiding principles (compile-time safety, terminal halt, provider + 4 hooks, hot-path discipline, observability by default, small capital first).
- Steady-state components: event/execution model, layered risk/safety, persistence (binary mandatory, QuestDB opt-in), providers (local/binance + future drift/), observability (TUI/ndjson/Prometheus future).
- Deferred: hedge, COIN-M, generic cross-margin, web UI (removed).

**[../architecture/migration.md](../architecture/migration.md)**: Chronological audit trail. Every core PR adds entry (date | desc | files | PR). Groups by phase. Records Phase 0 SOP, Phase 1 freeze artifacts/blocks/script, Phase 2.2 tiered MMR + funding wiring.

**[../architecture/perf-baseline.md](../architecture/perf-baseline.md) + [../architecture/performance.md](../architecture/performance.md)**: Detailed numerical anchor (36.67s baseline, StageTimer breakdown, memory, rings), append-only changelog of every locked optimization (with before/after, verification, reproduction steps), recommended order for remaining items (symbol interning largest, lock-free dashboard, shared_ptr audit, etc.). "0-2% wall = noise"; treat as regression targets.

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
- futures-order-lifecycle.md (10-step + brackets non-atomic note)
- killswitch-lan-unplug-timeline.md (network partition analysis)
- User data WS as source of truth, DMS orders-only.

**Note**: Aspirational SOPs use "Planned for Doc Phase X – current details live in docs/governance/01-prod.md". See reports/phase0/ for current evidence machinery.

---

## 16. Phase 0 Evidence Collection

Full details, templates, PROGRESS tracker, ritual live in `reports/phase0/` + [../governance/01-prod.md](../governance/01-prod.md). This section points only.

Use reports/phase0/PROGRESS.md (0/15), templates/, scripts/phase0/ for collection. See 01-prod.md for exit criteria.

---

## 17. C API & Embedding (planned reference/c-api.md; see aspirational in docs/README.md — current surface in CLAUDE + instructions)

Stable surface (opaque handle, JSON config same as engine_config):
- `tt_version()`, `tt_create_engine(config_json)`, `tt_run(handle)`, `tt_get_results()` (JSON, caller frees), `tt_last_error()`, `tt_free_string()`, `tt_destroy()`.
- nlohmann/json only at API boundary (not hot path).
- Same ENABLE_* and TT_TARGET gating (shared lib defaults backtest target; live only in engine_live binary).
- **Limitations today**: Batch backtest only. Shell out for live/replay/streaming. Full provider embedding future.
- Python ctypes example (context manager, TRUETEST_LIB discovery) in c-api.md.

---

## 18. Licenses & Third-Party (planned reference/licenses.md; current pins in CLAUDE.md + build files)

Authoritative table:
- Always: CLI11 (BSD), zstd (BSD chosen over GPL), nlohmann/json (MIT).
- Conditional: Boost (BSL for BINANCE/LIVE_DATA), OpenSSL (Apache for BINANCE), Abseil (Apache for DEBUG - never engine_live), GTest/Benchmark (tests only).
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

Full authoritative checklists, Phase 0 ritual, Phase 1 edit procedure, Go-Live Gate, DMS validation scenarios live in [../governance/01-prod.md](../governance/01-prod.md), [../governance/02-prerequisites.md](../governance/02-prerequisites.md), reports/phase0/, and CLAUDE.md.

**Technical pointers kept here**:
- Pre-merge: run freeze script, use correct model, no anti-patterns.
- Perf: 5+ median runs + update baseline.
- QuestDB health: soft warning ok (current).
- DMS / network drills: see killswitch timeline refs.

See governance for full "Go-Live Gate: all 9 rows...".

---

## 21. Master List of Critical Warnings & Non-Negotiables

Full governance warnings in [../governance/01-prod.md](../governance/01-prod.md) + CLAUDE.md. Key technical non-negotiables (preserved):

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
- Every PR touching core/risk/safety must reference [../governance/03-todo.md](../governance/03-todo.md) items (high-level) or docs/todos/XX e.g. docs/todos/02-P1-freeze.md#P1-02 and run freeze script.
- This instructions.md contains pointers + technical how-to (CLI, providers, build, flags, MC, QuestDB, realism, diagrams). Long-form phase/ritual/gates in [../governance/01-prod.md](../governance/01-prod.md).
- Phase 0/1 artifacts permanent in reports/phase0/.

**Related authoritative files**: [../CLAUDE.md](../CLAUDE.md), [../governance/01-prod.md](../governance/01-prod.md), [../governance/02-prerequisites.md](../governance/02-prerequisites.md), [../governance/03-todo.md](../governance/03-todo.md), [../governance/04-summary.md](../governance/04-summary.md) + reorganized `docs/reference/` + `docs/README.md`. Historical in `docs/archive/`. Phase 0 evidence in `../reports/phase0/`.

Aspirational refs use explicit "Planned for Doc Phase X – current in docs/governance/01-prod.md".

**docs/ is now the central authoritative documentation home.**

---

**End of Master Consolidated Instructions.** (Slimmed 2026-07 for duplication reduction.)

*Last updated: 2026-07 (docs overhaul)*
