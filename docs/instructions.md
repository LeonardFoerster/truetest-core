# TrueTest / hft-engine — Master Consolidated Instructions

**Status**: Single authoritative reference. Produced via multi-agent exhaustive extended-thinking analysis of every Markdown file in the repository. This document lives alongside a reorganized documentation set (see [docs/README.md](README.md) for the current structure). It supersedes the scattered prior documents while preserving all key substance.

**Version**: 1.0 (Consolidated 2026-05 from 4 parallel deep-dive subagent reports + source cross-checks)  
**Philosophy**: "Someone reading only one phase still has the context of the invariants." Deliberately comprehensive and repetitive on safety. Update after every phase or material change.

**Quick Links (Internal)**: [Philosophy & Invariants](#1-philosophy-invariants-safety-model) • [Production Phases](#3-production-readiness-phases) • [Build](#5-build-cmake-reference) • [CLI Reference](#6-complete-cli-flags-json-config) • [Live Futures SOP](#15-operator-sops-testnet-guides) • [Phase 0 Evidence](#16-phase-0-evidence-collection) • [Critical Warnings](#21-master-list-of-critical-warnings-non-negotiables) • [All Checklists](#20-consolidated-checklists)

---

## 1. Philosophy, Invariants, Safety Model

TrueTest is a modular C++23 engine for reproducible backtesting, divergence-aware shadow trading, and gated live execution from a **single source tree**. Three binaries (`engine_backtest`, `engine_shadow`, `engine_live`) differ only by the compile-time `TT_TARGET` define in `src/core/tt_target.h`. Live-order paths are physically removed via dead-code elimination in non-live targets (`target_allows_live_orders()` is constexpr false).

**Core non-negotiable invariants** (repeated across governance, architecture, user-manual, futures docs, killswitch timeline, [architecture/target-architecture.md](architecture/target-architecture.md), [architecture/MODEL.md](architecture/MODEL.md), CLAUDE.md, prod.md):

1. **Compile-time live-order gate is absolute** — Only `engine_live` (and future keeper_live targets) can ever emit real orders/transactions. Never introduce runtime `allow_live_orders` checks or recompile-time bypasses.
2. **Halt is terminal** (`halt_flag_` in engine/risk) — Write-once atomic true; only manual operator intervention + explicit process restart clears it. No auto-resume, no SIGUSR1, no cooldown, no "helpful" retry logic on safety paths.
3. **Safety paths are loud and non-retrying** — Kill-switch, DMS, reconciler, WorkerWatchdog failures escalate to operator with clear diagnostics. No silent backoff/retry on safety surfaces.
4. **Hot-path discipline** (enforced by `scripts/check-hotpath-json.sh` + layer-deps) — Zero `nlohmann::json` on hot path; zero (or object-pool) allocations on event loop; lock-free SPSC RingBuffer (65536 slots, exactly one producer/consumer); no second writer on any ring.
5. **Reconciler refusal is default** — Blocks startup on position/order drift > tolerance (configurable `--reconcile-tolerance-bps`). Only documented soft-warn exception: spot testnet monthly account resets (futures has none).
6. **User-data WebSocket is source of truth** (Binance futures/spot) — `ORDER_TRADE_UPDATE` + `ACCOUNT_UPDATE` after initial REST ack. Position snapshots from REST are advisory only until reconciled.
7. **DMS protects orders only** (`/fapi/v1/countdownCancelAll`) — Venue-side auto-cancel on heartbeat loss. Does **not** emit reduceOnly MARKET flattens (Phase 3 work). Kill-switch (orderly) does cancel-all + reduceOnly flatten with hard deadline.
8. **Futures mandates** — One-way mode hard refusal in `BinanceFuturesProvider::open()`; `reduceOnly` + `closePosition=true` brackets (non-atomic, two POSTs with auto-cancel guarantee); pre-trade venue `FuturesRiskCheck` (notional/leverage/liq-distance + real tiered MMR from `/fapi/v1/leverageBracket`) consulted **before** `RiskManager` in hot path (`engine.cpp:1600-1628`).
9. **Provider abstraction is the sole extension point** — `IProvider` + four safety hooks (`IReconciler`, `IKillSwitch`, `IRiskCheck`, `IBracketAdapter`) + transport/parser/executor. Core engine never contains `#ifdef HAS_*` or venue specifics.
10. **Small capital first + evidence-based gates** — Every phase exit requires artifacts (binary logs, QuestDB run_tag, signed notes, shadow reports) + two-person sign-off before capital tier increase. "No capital tier increase is permitted until all nine rows [Go-Live Gate] have two signatures."

**Additional strong primitives already present**: Layered risk (venue first), `WorkerWatchdog` (3× heartbeat), clock-skew/WAF/symbol existence/one-way probes at open, `ExecutionBridge` mutex audit, rate limiter, per-lot `Portfolio` + `ExitManager`, binary zstd event log + replay, QuestDB (soft-fail today), rich ncurses TUI, StageTimer/ring stats observability.

**Philosophy quote** (prod.md): "The engine already implements strong compile-time and runtime safety primitives that most retail or early-stage systems lack... This playbook exists so that future operators cannot say 'we forgot why we were careful.'"

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

**Phase 1 Live-Safety Freeze** (prod.md + CLAUDE + prerequisites.md + decisions/phase1-freeze-template.md + scripts/check-live-safety-freeze.sh):

- 10 protected files carry `LIVE-SAFETY SURFACE — Phase 1 freeze` comment blocks (tt_target.h, engine.cpp + risk paths, binance_futures_provider.h live block ~224-410, dead_mans/kill/reconciler, risk_manager/futures_risk_check, live_safety.h, worker_watchdog.h, etc.).
- **Mechanical gate**: Any edit to frozen surface requires commit message containing `LIVE_SAFETY_CCB_APPROVED` token.
- Enforcement: `scripts/check-live-safety-freeze.sh` (pre-commit + CI; fails on HEAD for historical violation); two-person CCB review + clean 4-hour (or 8-hour mainnet shadow) `engine_shadow` run before merge.
- Prerequisites checklist (prerequisites.md) **must be green** before any deepdive/refactor touching core/risk/safety/live paths. Includes reading CLAUDE+prod, running the check script, no open untagged changes, Phase 0 still in progress, etc.
- Every relevant PR must note "prod.md impact" and reference relevant todo items.

**Pre-merge safety checklist** ([architecture/MODEL.md](architecture/MODEL.md) copy-paste into PRs): model used, target_allows_live not bypassed, halt terminal, hotpath-json + layer-deps scripts pass, no new retry/soft-warn/HAS_/second-producer/runtime gate, etc.

---

## 3. Production Readiness Playbook & Capital-Tier Phases (prod.md)

Strict rule: Moving tiers requires prior phase exit criteria + two-person sign-off on the 9-row Go-Live Gate table.

### Phase 0 — Safe Tiny-Size Mainnet Futures (Current Active)
- **Exact command template** (conservative; must meet or exceed):
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
- **Why each flag mandatory**: depth for L2/queue models, persist for audit, DMS liveness, reconciler, three futures risk caps, daily loss + unwind.
- **Exit criteria**: 15+ fully documented qualifying sessions across ≥3 volatility regimes (High/Med/Low via 7/14d BTC realized vol), zero unexplained drift > tolerance, **full artifacts** for every session (zstd .bin, QuestDB run_tag, signed one-page note from template, row in PROGRESS.md, post-halt grep review), two-person batch reviews every 5.
- **Ritual** (futures-phase0-operator-sop.md + reports/phase0/*): Print/sign SOP, `new-session.sh`, math-captcha visible entire session, stay at terminal, one-way mode confirmed in UI + provider, DMS counter advancing in TUI, post-halt mandatory `grep -i "POSITION-SNAPSHOT|funding|drift"`, `post-session.sh`, volatility classifier, commit under reports/phase0/.
- **Status** (2026-05): 0/15 qualifying; helpers/scripts exist; in active collection. Use `reports/phase0/PROGRESS.md` as single source of truth.

### Phase 1 — Deepdive Stabilization & Live-Safety Freeze
- Planning artifacts created ([architecture/target-architecture.md](architecture/target-architecture.md), [architecture/migration.md](architecture/migration.md), todo.md, prerequisites.md).
- LIVE-SAFETY blocks + enforcement script + CLAUDE update done.
- Remaining: clean 8h mainnet `engine_shadow` (0 drops), two-person sign-off in `decisions/phase1-freeze-*.md`, prod.md/todo update.
- **All future edits** to frozen surface require token + CCB + shadow run.

### Phase 2 — Risk Engine Completion (Highest Impact)
- Funding as first-class (`funding_event` in event.h, portfolio::on_funding, QuestDB/analytics/risk/CBs, TUI).
- Real tiered liquidation (`MaintenanceMarginTable` from `/fapi/v1/leverageBracket`, hot-patch setter into FuturesRiskCheck) — **implemented + build-fixed 2026-05**.
- Position sizing % equity + volatility; circuit breakers (spread, funding rate).
- Status: 2.1/2.2 complete; 2.3/2.4 pending.

### Phases 3–6 (High-Level)
- 3: DMS position flattening (`reduceOnly` MARKET on expiry) + external `tt_watchdog` binary.
- 4: `--persist-strict` (hard-fail), mandatory binary log + xxhash integrity, richer checkpoints (full `lots_` map), crash-replay golden test.
- 5: Prometheus + IAlertSink, encrypted credential store, runbooks.
- 6: 60+ day continuous mainnet shadow divergence report, post-mortems for every halt/incident, CCB charter + decision log, signed exit review.

**Final Go-Live Gate Table** (9 rows, all require two signatures + concrete evidence):
1. All prior phases met.
2. 60-day shadow report.
3. Funding + tiered exercised 30d.
4. DMS flatten tested.
5. persist-strict + creds on 10 sessions.
6. Prometheus alert drill.
7. Runbooks walked.
8. CCB size-increase approved.
9. Independent safety review.

---

## 4. Prerequisites, Change Control, Task Tracking

See prerequisites.md for the living Phase 1+ checklist (must be green before PRs touching frozen surface). Run `./scripts/check-live-safety-freeze.sh --check-head`, tick boxes, reference in PR.

todo.md is the phased task list (current Phase 1 items, future 2–6 bullets). Every frozen PR must reference relevant items. Update after phase completion.

reports/phase0/ contains the evidence machinery (README for layout, PROGRESS.md tracker, PHASE0_COMPLETION_PLAN for campaign details, ops/ for batch reviews, templates/ for session notes).

---

## 5. Build & CMake Reference (from instructions.md + CLAUDE + perf docs)

**Minimal (zero external deps)**:
```bash
cmake -B build
cmake --build build
```
Produces `engine_backtest` (default), `engine_shadow`, `engine_live` (TT_TARGET=1/2/3).

**Full-featured**:
```bash
cmake -B build \
  -DENABLE_BINANCE=ON \
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

**Key CMake Flags** (see instructions.md §3 for exhaustive table):
- Feature: ENABLE_BINANCE, ENABLE_QUESTDB, ENABLE_LIVE_DATA, ENABLE_DEBUG (Abseil), ENABLE_BENCHMARKS.
- Build: CMAKE_BUILD_TYPE=Release (with DEBUG for instrumentation), ENABLE_NATIVE_OPT, BUILD_TESTS/SHARED_LIB.
- Sanitizers (Debug, mutually exclusive where noted): ENABLE_TSAN (preferred for threading), ASAN+UBSAN combos (OPTIONS="halt_on_error=1,abort_on_error=1,...").
- Perf reference build: Release + ENABLE_DEBUG + NATIVE_OPT + BENCHMARKS.

**Build audit header**: Every binary prints `AUDIT: git=... timestamp=... pins=...` (truetest_version.h generated).

**Presets, Install, Packaging**: See instructions.md §8-10 (linux-default preset, CPack TGZ/DEB).

**FetchContent pins** (CLI11, zstd, nlohmann/json, etc.) and license rules in [reference/licenses.md](reference/licenses.md) (no copyleft, Abseil never in engine_live, hot-path JSON CI gate).

**Performance build/instrumentation** ([architecture/engine-optimization.md](architecture/engine-optimization.md) + [architecture/performance.md](architecture/performance.md) + [architecture/perf-baseline.md](architecture/perf-baseline.md)):
- Reference workload: 50k-bar synthetic CSV, SMA, inline preset (baseline ~36.67s wall, 1.4k ev/s on Ryzen 9 5900X; 5+ median runs).
- Instrumentation: StageTimer (9 stages: market_create, strategy, orderbook, fill, ring_publish, risk_check, mm_replenish, ...), ring_stats (drops/HWM critical, 0 drops required in prod), memory/copy trackers.
- Investigation: reproduce → read StageTimer/ring/copy → microbench → lock with baseline update in [architecture/perf-baseline.md](architecture/perf-baseline.md) + regression guard.
- Historical wins (locked): mimalloc (tails −29%), PGO (layout/tails −47–97% on cheap stages), absl::flat_hash_map (maps), deque→ring + prealloc scratch (mm_replenish), etc. Dominant remaining cost: mm_replenish + market_create alloc volume (~1M make_shared<order> per 50k bars).

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

**TUI**: Rich ncurses tabbed dashboard on shadow/live (positions, orders, L2, risk, brackets, debug StageTimer/ring, health/DMS counter). Hotkeys, setup menu on backtest. `--no-tui` for headless/CI.

**JSON config**: Full engine_config schema (mode, provider, strategy, risk, threading, persistence, realism, etc.). See instructions §13 for keys.

**start.sh launcher** and many numbered examples in instructions §29–35 (backtest minimal → full futures live with DMS/persist/risk caps → sanitizers → PGO training → replay → QuestDB queries, etc.).

---

## 7. Providers, Data Sources, Realism Models, Orderbook (consolidated)

**Providers** (`IProvider`):
- `local`: CSV OHLCV (bar) or tick-level; BinaryCache decorator; multi-path.
- `binance` / `binance-futures`: Combined trade + depth WS, REST execution (HybridExecutor paper/shadow, signed REST + user-data WS live), L2 seeding for realism when `--depth-stream`.
- Replay: binary event log or `--replay-data`.
- Future: `drift` (see upcoming plans).

**Data validation + formats**: Strict schema checks; see instructions §19.

**Realism models** ([architecture/realism.md](architecture/realism.md) — all default off, require `--depth-stream` for L2-dependent, **completely bypassed in live**; live venue supplies truth):
- `--realistic-fills`: passive/resting prices, one fill_event per level walked.
- Latency: two layers (`latency_model` strategy→eligible, `wire_latency_model` order→venue).
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
7. Fill → Portfolio (per-lot) + ExitManager brackets + Analytics + QuestDB + rings.
8. Bracket handling (two POSTs).
9. Shutdown/kill-switch (cancel-all + reduceOnly MARKET, hard deadline).
10. DMS (independent heartbeat + venue countdownCancelAll).

**Where orders can fail** table (with catcher layer) in futures-order-lifecycle.md.

---

## 9. Risk Management, DMS, Kill-Switch, Reconciler, WorkerWatchdog (detailed in futures-testnet.md, killswitch timeline, prod, user-manual)

**Layered**:
- Venue `IRiskCheck` / `FuturesRiskCheck` (notional, leverage, liq-distance, real tiered MMR) — hot path, before RiskManager.
- RiskManager (balance, daily loss, % equity/vol sizing, spread/funding CBs → halt).
- Startup IReconciler (position/order vs /positionRisk + availableBalance; refusal default).
- Shutdown IKillSwitch (cancel-all + reduceOnly flatten, deadline).
- DMS (countdownCancelAll heartbeat; protects orders only; WorkerWatchdog monitors; 3× heartbeat internal).
- Halt propagation to all workers + ring policy `halt_on_drop` on safety rings.

**Futures testnet DMS validation playbook** (futures-testnet.md — 5 scenarios A–E with tables, conservative caps command, aliases `bf-orders`/`bf-position`, pass/fail, recording):
- A: Clean SIGINT.
- B: SIGKILL.
- C: OOM simulation.
- D: Network unplug (physical or iptables).
- E: SIGSTOP (foot-gun demo showing DMS still fires).

**Killswitch LAN-unplug timeline** (killswitch-lan-unplug-timeline.md — canonical catastrophic disconnect analysis):
- Blocking `net::connect()` in REST client (tcp_syn_retries=6 → 60–90s hang) is a known foot-gun.
- WS keepalive/Beast timeout → fatal → halt.
- DMS venue timer still fires and cancels orders (~30s).
- Kill-switch and DMS flatten require the lost network → wedge.
- Implications: unplug validates DMS + halt story, not flatten (use `--dms-attempt-position-close` + independent machine for drill). Phase 3 external watchdog + non-blocking connect needed.

**Refusal modes table** (clock skew, hedge, symbol not found, permissions, reconciler drift, etc.) in futures-testnet.md.

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

**[architecture/target-architecture.md](architecture/target-architecture.md)** (north star, Phase 1 artifact):
- 6 guiding principles (compile-time safety, terminal halt, provider + 4 hooks, hot-path discipline, observability by default, small capital first).
- Steady-state components: event/execution model, layered risk/safety, persistence (binary mandatory, QuestDB opt-in), providers (local/binance + future drift/), observability (TUI/ndjson/Prometheus future).
- Deferred: hedge, COIN-M, generic cross-margin, web UI (removed).

**[architecture/migration.md](architecture/migration.md)**: Chronological audit trail. Every core PR adds entry (date | desc | files | PR). Groups by phase. Records Phase 0 SOP, Phase 1 freeze artifacts/blocks/script, Phase 2.2 tiered MMR + funding wiring.

**[architecture/perf-baseline.md](architecture/perf-baseline.md) + [architecture/performance.md](architecture/performance.md)**: Detailed numerical anchor (36.67s baseline, StageTimer breakdown, memory, rings), append-only changelog of every locked optimization (with before/after, verification, reproduction steps), recommended order for remaining items (symbol interning largest, lock-free dashboard, shared_ptr audit, etc.). "0–2% wall = noise"; treat as regression targets.

---

## 14. Strategy Validation Roadmap (strategy-validation.md)

Four ranked future deliverables (mostly planned):
1. Dual-portfolio shadow (second Portfolio in engine_shadow consuming exchange_filled; sim vs exch equity/PnL/Sharpe/DD at shutdown; QuestDB suffixed; highest leverage).
2. A/B comparison CLI (Python + QuestDB queries, side-by-side + curves + bootstrap CI).
3. Per-trade analyzer (round-trips, MAE/MFE, slippage vs price series).
4. L2-aware queue position modeling in TradeTapeShadowAdapter + schema column.

Acceptance commands and methodology in the doc. Ties to realism + demo-trading-workflow.

---

## 15. Operator SOPs, Testnet Guides, Demo Workflows, Killswitch Timeline (full operational ladder)

**Recommended 5-step pre-mainnet validation path** (repeated across futures-testnet.md, demo-trading-workflow.md, user-manual, futures-phase0-operator-sop.md):
1. Backtest + realism models on recorded real mainnet futures tape.
2. Deterministic replay.
3. Live mainnet shadow (`TradeTapeShadowAdapter` + ShadowTracker for sim vs exchange divergence).
4. Protocol/wire + safety validation on futures testnet (DMS playbook A–E, one-way, recon, refusals; **never calibrate realism here** — synthetic liquidity).
5. Tiny-size Phase 0 mainnet under official SOP + full artifacts.

**futures-testnet.md** (USDT-M): Account one-way mode, `--testnet`, math-captcha skipped, pre-trade caps + liquidation projection math, DMS details + 5-scenario playbook, refusal modes table, what engine does (recon, kill, brackets), gotchas (thin book, resets, partial brackets declined), integration smoke test.

**testnet.md** (spot counterpart): Simpler; WAF client_order_id guard; reconciler reset tolerance for monthly wipes; pointer to futures doc.

**demo-trading-workflow.md**: Exact record (`--record --persist --run-tag`), replay, shadow commands (futures variants with `--wire-latency-us`), QuestDB inspection, acceptance criteria (determinism, bounded divergence, P&L tracking). Futures adaptations box.

**futures-order-lifecycle.md**: 10-step narrative + differences vs spot + failure catchers table. "User-data WS is source of truth after initial ack." "Brackets on futures are not atomic." "DMS protects against sudden death (but does not close positions)."

**killswitch-lan-unplug-timeline.md**: Brutally honest postmortem of physical network partition. Timeline table (t=0 to >70s), what actually protected (DMS orders-only), implications for drills, recommended test procedure with `--dms-attempt-position-close` + independent machine, open gaps (blocking connect, no external watchdog yet).

**user-manual.md**: Primary overview + architecture diagram + risk/safety features + full CLI examples + limitations + file index.

**grok.md**: Implementation log for dual-portfolio shadow P&L (status, design, future phases).

---

## 16. Phase 0 Evidence Collection, Templates, PROGRESS (reports/phase0/*)

- **PROGRESS.md**: Master tracker table (#, Date, Run Tag, Symbol, Regime, Notional Cap, Daily Loss, Drift bps, Artifacts, Retro?, Reviewed, Notes). 0/15 qualifying. Update after every session. Batch review log every 5.
- **PHASE0_COMPLETION_PLAN.md**: Detailed campaign (prep + 3 batches of 5 + buffer, per-session workflow with scripts/phase0/new-session.sh + post-session.sh + analyze-log.sh, volatility targeting, abort conditions, deliverables, sign-off).
- **templates/phase0-session-note.md**: One-page signed note (redacted command, metrics, incident log, declaration checkboxes for safety wires/captcha/no unexplained drift/artifacts, signatures).
- **ops/batch-review-template.md**: Per-batch package (summary table, regime coverage, evidence checklist per session, observations, volatility method, dual sign-off).
- **ops/volatility-log.md**: Thresholds (High >60% or spike, Med 35-60, Low <35 via TradingView/Binance 7/14d BTC vol) + table + `./scripts/phase0/volatility-classifier.sh`.
- **scripts/phase0/**: new-session, post-session, analyze-log, volatility-classifier, create-evidence-bundle, dry-run-phase0, etc.

**Volatility regime labeling** required for every qualifying session. Retroactive credit rules defined.

---

## 17. C API & Embedding ([reference/c-api.md](reference/c-api.md))

Stable surface (opaque handle, JSON config same as engine_config):
- `tt_version()`, `tt_create_engine(config_json)`, `tt_run(handle)`, `tt_get_results()` (JSON, caller frees), `tt_last_error()`, `tt_free_string()`, `tt_destroy()`.
- nlohmann/json only at API boundary (not hot path).
- Same ENABLE_* and TT_TARGET gating (shared lib defaults backtest target; live only in engine_live binary).
- **Limitations today**: Batch backtest only. Shell out for live/replay/streaming. Full provider embedding future.
- Python ctypes example (context manager, TRUETEST_LIB discovery) in c-api.md.

---

## 18. Licenses & Third-Party ([reference/licenses.md](reference/licenses.md))

Authoritative table:
- Always: CLI11 (BSD), zstd (BSD chosen over GPL), nlohmann/json (MIT).
- Conditional: Boost (BSL for BINANCE/LIVE_DATA), OpenSSL (Apache for BINANCE), Abseil (Apache for DEBUG — never engine_live), GTest/Benchmark (tests only).
- QuestDB: zero new deps (raw POSIX).
- Rules: No vendored trees, copyleft forbidden (dual-license election documented), update table + pins in same PR, engine_live column, hot-path JSON CI gate.

---

## 19. Future Directions & Upcoming (upcoming/*.md + prod phases + target)

**Drift Protocol Liquidation Keeper Bot** (claude_drift.md + grokd_drift.md — two detailed overlapping plans, May 2026):
- Non-speculative infrastructure play (inventory risk on unwind acknowledged; small capital limits scope).
- Engine reuse ~25-30% (IProvider + 4 safety hooks, rings, WorkerWatchdog, DMS/Kill/Reconciler patterns, event log/replay, QuestDB/TUI/config, Boost.Beast/OpenSSL). Do not reuse orderbook/strategies/Binance specifics.
- Recommended: Hybrid C++ engine + Rust FFI (`drift-rs`) for decode/margin/tx build (Solana not natural in C++).
- 7-phase plan (both docs): Phase 0 skeleton (ENABLE_DRIFT + keeper_sim/keeper_live targets + run_keeper() + FFI stub), 1 (multi-RPC + simulateTransaction mandatory), 2 (UserMap + exact margin health via FFI — hardest), 3 (profitability sim post-unwind), 4 (tx build v0+ALT + Jito + unwind queue), 5 (capital caps + adapted safety + external supervisor mandatory vs SIGSTOP), 6 (systemd + alerting), 7 (replay/shadow → tiny rollout with 70-80% shadow success gate).
- Cross-cutting: simulation gate non-negotiable, resubscribe-on-reconnect, dedicated RPCs, external supervisor, exact on-chain math.
- Bottom line: Shadow run (economic model) is make-or-break before any capital.

**prod.md Phases 3–6** (DMS flatten + external watchdog; persist-strict + richer checkpoints + mandatory log; Prometheus/alerts/creds; 60d report + post-mortems + CCB charter).

**Other deferred** (target-architecture): hedge mode, COIN-M, generic multi-venue cross-margin, keeper mode (`run_keeper()`), Prometheus.

---

## 20. Consolidated Checklists & Procedures (from all sources)

**Phase 1 Freeze Edit Procedure**:
1. Run `./scripts/check-live-safety-freeze.sh --check-head` (green).
2. Tick prerequisites.md boxes (read CLAUDE+prod, etc.).
3. Use Opus for safety surface.
4. Commit with `LIVE_SAFETY_CCB_APPROVED` token + "prod.md impact: ..." note.
5. Two-person CCB + 4h/8h clean mainnet shadow before merge.
6. Sign decisions/phase1-freeze-*.md.

**Phase 0 Qualifying Session Ritual** (print/sign futures-phase0-operator-sop.md every time):
- Pre: new-session.sh, export keys, open math-captcha, confirm one-way in Binance UI, verify DMS counter advancing.
- Command: use (or exceed) the conservative template above.
- During: physical presence entire session; monitor TUI (DMS, risk, snapshots, health); math-captcha visible (mainnet).
- Post-halt: mandatory `grep -i "POSITION-SNAPSHOT|funding|drift" <event-log>` before resume.
- Post: post-session.sh, human fills/signs note, append PROGRESS row, commit artifacts (zstd log, QuestDB, note), classify regime, batch review every 5.

**Pre-Merge Safety (MODEL.md)**: Diff with Opus (or human), target gate not bypassed, halt terminal, scripts pass, no new anti-patterns, model noted.

**Go-Live Gate**: All 9 rows two signatures + evidence.

**Perf Locking**: Reference 5+ median runs on workload → update perf-baseline.md + regression guard.

**DMS Validation (testnet)**: Run 5 scenarios A–E with independent monitoring + recording.

**Network Partition Drill**: Use `--dms-attempt-position-close` + independent machine + post-reconcile + exact timing log.

**QuestDB Health**: Watch for soft warning; continue disabled is acceptable today.

**Volatility + Batch Review**: Use scripts + templates; dual sign-off.

---

## 21. Master List of Critical Warnings & Non-Negotiables (consolidated from all)

- Never increase capital tier without prior phase exits + full Go-Live Gate sign-offs.
- Never edit frozen safety surface without token + CCB + shadow run (script enforces).
- Halt is terminal — any code suggesting resume/retry/cooldown on halt paths is rejected.
- No JSON on hot path; no allocations where possible; SPSC discipline strict.
- DMS **does not close positions** (orders only) — operator/external watchdog must handle flatten.
- SIGSTOP/SIGKILL/suspension defeats in-process kill-switch; DMS still fires (venue timer) but positions remain.
- Blocking TCP connect in REST client causes long hangs on network loss — engine appears hung; kill-switch/DMS flatten require the lost network.
- One-way mode is **hard refusal** on futures; hedge mode prevents start.
- Reconciler refusal default — do not force; investigate.
- Math-captcha window must remain visible and attended entire mainnet live Phase 0+ session.
- Stay at terminal; do not leave machine unattended with live orders.
- Testnet: never calibrate realism/slippage/impact (synthetic liquidity, fictional funding, thin books, resets). Use only for wire/protocol/DMS validation.
- Post-any-halt: mandatory event-log grep review before resume or new orders.
- QuestDB soft-fail today (data loss possible on daemon loss); hard-fail future.
- Position snapshots advisory until full recon design.
- Funding ignored in P&L/risk today (mitigated in partial Phase 2).
- Brackets non-atomic on futures.
- Realism models + queue/impact/latency **bypassed in live**; only honest for backtest/shadow divergence measurement.
- Ring drops on safety rings → halt_on_drop.
- Agent cannot trade — human only for live capital decisions.
- When in doubt: flatten + review logs.
- Sonnet on Opus-zone safety files risks shipping subtle invariant violations.
- External `tt_watchdog` + non-blocking connect + richer checkpoints not yet present (Phase 3/4 gaps).
- Small capital + full artifacts + eyes-open only until Phase 0/1 exit.

---

## 22. Source Code Cross-References (key locations from all docs)

- `src/core/tt_target.h` (TT_TARGET, target_allows_live_orders, frozen).
- `src/engine/engine.{h,cpp}` (hot loop, process_order ~1600-1628 risk, LocalBookAdapter ctor ~203, l2_seeded ~1721, on_exchange_fill ~2343, log_event sites, build_dashboard_view, publish, restore_state, pending_orders_, mm_replenish, StageTimer sites, dual-portfolio wiring, shutdown, halt propagation).
- `src/providers/binance/` (32 files): binance_futures_provider.h (open ~224-410, position_snapshot ~366-375, funding log ~647-655, leverageBracket, one-way probe, advisories), dead_mans_switch.h (class comment ~31-33), kill_switch.h, reconciler.h, safety.h, bracket_adapter, user_data_parser (funding reason), rest_client (blocking connect), combined parser, ExecutionBridge.
- `src/risk/` (risk_manager, futures_risk_check ~154-176 approx, maintenance_margin_table).
- `src/execution/` (portfolio (lots_, on_funding), execution_adapter (LocalBookAdapter, QueueAware), latency/impact/queue_model/queue_position_model, trade_tape_shadow_adapter, live_safety, order_tracker, fee_model, etc.).
- `src/orderbook/`, `src/exits/` (ExitManager, bracket_adapter), `src/strategy/` (registry), `src/analytics/` (shadow_tracker, adverse_selection, on_funding).
- `src/data/questdb/` (store, ilp_writer, schema, http/tcp_client, run_tag — current direct mutex impl).
- `src/threading/` (ring_buffer, worker_watchdog, thread_preset, worker).
- `src/debug/` (stage_timer, ring_stats, memory_info, copy_tracker).
- `src/core/event.h` (funding_event), `src/bin/main.inc` (CLI wiring, defaults DMS 30s/10s, kill 5s).
- `src/api/truetest_api.{h,cpp}`.
- `CMakeLists.txt`, `cmake/{Dependencies,CompilerFlags}.cmake`, `scripts/{check-live-safety-freeze.sh, check-hotpath-json.sh, phase0/*, pre-commit}`.
- `tests/` (golden_regression, binance_futures_testnet_live, questdb_*, impact/latency/queue, etc.).

Cross-references point to files now organized under `architecture/`, `operations/`, `reference/`, plus the root governance files and `reports/phase0/`. See [docs/README.md](README.md) for the current layout.

---

## 23. Document Maintenance & Evolution

- Living documents — update after every phase completion, before capital tier increase, and after any material change to frozen surface.
- Every PR touching core/risk/safety/live-provider must add migration.md entry and reference todo/prod impact.
- This master instructions.md is now the single source; other .md files may be archived or point here.
- Phase 0/1 artifacts (reports/phase0/, decisions/phase1-freeze-*.md) are permanent evidence.
- Minor sync lags (e.g., "9 files" vs 10 in enforcement, gaps.md vs prod Phase 2 status, stale QuestDB worker text in instructions) are evolution artifacts — trust the most recent prod/CLAUDE + db.md reality + this master.

**Related authoritative files**: Root governance ([CLAUDE.md](../CLAUDE.md), [prod.md](../prod.md), [prerequisites.md](../prerequisites.md), [todo.md](../todo.md)), plus the reorganized set under `docs/` (see [docs/README.md](README.md)). Historical material lives in `docs/archive/`. Phase 0 evidence is in `../reports/phase0/`.

---

**End of Master Consolidated Instructions.** All content from the original corpus has been read, deeply analyzed by multiple agents with extended cross-referenced thinking, and unified here for completeness. Use this document for all operator, developer, reviewer, and production decisions. For the absolute latest code state, always verify against HEAD + the enforcement scripts.

*Generated 2026-05 via parallel subagent synthesis of the full Markdown corpus.*