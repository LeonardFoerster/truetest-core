# TrueTest / hft-engine — Master Consolidated Instructions

**Status**: Single authoritative reference. Produced via multi-agent exhaustive extended-thinking analysis of every Markdown file in the repository. This document lives alongside a reorganized documentation set (see [docs/README.md](README.md) for the current structure). It supersedes the scattered prior documents while preserving all key substance.

**Version**: 1.0 (Consolidated 2026-05 from 4 parallel deep-dive subagent reports + source cross-checks)  
**Philosophy**: "Someone reading only one phase still has the context of the invariants." Deliberately comprehensive and repetitive on safety. Update after every phase or material change.

**Quick Links (Internal)**: [Philosophy & Invariants](#1-philosophy-invariants-safety-model) • [Production Phases](#3-production-readiness-phases) • [Build](#5-build-cmake-reference) • [CLI Reference](#6-complete-cli-flags-json-config) • [Live Futures SOP](#15-operator-sops-testnet-guides) • [Phase 0 Evidence](#16-phase-0-evidence-collection) • [Critical Warnings](#21-master-list-of-critical-warnings-non-negotiables) • [All Checklists](#20-consolidated-checklists)

---

## 1. Philosophy, Invariants, Safety Model

TrueTest is a modular C++23 engine for reproducible backtesting, divergence-aware shadow trading, and gated live execution from a **single source tree**. Three binaries (`engine_backtest`, `engine_shadow`, `engine_live`) differ only by the compile-time `TT_TARGET` define in `src/core/tt_target.h`. Live-order paths are physically removed via dead-code elimination in non-live targets (`target_allows_live_orders()` is constexpr false).

**Core non-negotiable invariants** (repeated across governance, `prod.md`, user-manual, CLAUDE.md; detailed target architecture + MODEL.md + realism.md etc. planned under `docs/architecture/` — Doc Phase 2 / D-03; current authoritative in prod.md + CLAUDE + instructions + user-manual):

1. **Compile-time live-order gate is absolute** — Only `engine_live` (and future keeper_live targets) can ever emit real orders/transactions. Never introduce runtime `allow_live_orders` checks or recompile-time bypasses.
2. **Halt is terminal** (`halt_flag_` in engine/risk) — Write-once atomic true; only manual operator intervention + explicit process restart clears it. No auto-resume, no SIGUSR1, no cooldown, no "helpful" retry logic on safety paths.
3. **Safety paths are loud and non-retrying** — Kill-switch, DMS, reconciler, WorkerWatchdog failures escalate to operator with clear diagnostics. No silent backoff/retry on safety surfaces.
4. **Hot-path discipline** (enforced by `scripts/check-hotpath-json.sh` + layer-deps) — Zero `nlohmann::json` on hot path; zero (or object-pool) allocations on event loop; lock-free SPSC RingBuffer (65536 slots, exactly one producer/consumer); no second writer on any ring.
5. **Reconciler refusal is default** — Blocks startup on position/order drift > tolerance (configurable `--reconcile-tolerance-bps`). Only documented soft-warn exception: spot testnet monthly account resets (futures has none).
6. **User-data WebSocket is source of truth** (Binance futures/spot) — `ORDER_TRADE_UPDATE` + `ACCOUNT_UPDATE` after initial REST ack. Position snapshots from REST are advisory only until reconciled.
7. **DMS protects orders only** (`/fapi/v1/countdownCancelAll`) — Venue-side auto-cancel on heartbeat loss. Does **not** emit reduceOnly MARKET flattens (Phase 3 work). Kill-switch (orderly) does cancel-all + reduceOnly flatten with hard deadline.
8. **Futures mandates** — One-way mode hard refusal in `BinanceFuturesProvider::open()`; `reduceOnly` + `closePosition=true` brackets (non-atomic, two POSTs with auto-cancel guarantee); pre-trade venue `FuturesRiskCheck` (notional/leverage/liq-distance + real tiered MMR from `/fapi/v1/leverageBracket`) consulted **before** `RiskManager` in hot path (`engine.cpp:1600-1628`).
9. **Provider abstraction is the sole extension point** — `IProvider` + four safety hooks (`IReconciler`, `IKillSwitch`, `IRiskCheck`, `IBracketAdapter`) + transport/parser/executor. Core engine never contains `#ifdef HAS_*` or venue specifics.
10. **Small capital first + evidence-based gates** (personal use only) — When the author chooses to collect evidence toward personal live use, every phase exit requires artifacts (binary logs, QuestDB run_tag, signed notes, shadow reports) + two-person sign-off before any personal capital tier increase. "No personal capital tier increase is permitted until all nine rows [Go-Live Gate] have two signatures."

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

## 3. Production Readiness Playbook & Capital-Tier Phases

**Full authoritative version**: see [`prod.md`](prod.md) (the central production contract).

`prod.md` contains:
- Philosophy and the 9 core invariants
- Exact Phase 0 command template + exit criteria + ritual
- Phase 1 freeze rules and remaining work
- Phases 2–6 roadmap
- The 9-row Go-Live Gate table (all rows require two signatures + evidence)

### Quick Reference – Phase 0 Command Template (from prod.md)
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

See `prod.md` for the complete "why each flag" explanation, full exit criteria, ritual, and Go-Live Gate.

---

## 4. Prerequisites, Change Control, Task Tracking

See [`prerequisites.md`](prerequisites.md) (living Phase 1+ checklist – must be green before any PR touching the frozen safety surface). Always run `./scripts/check-live-safety-freeze.sh --check-head` and include the result.

See [`todo.md`](todo.md) for the current phased task list. Every frozen-surface PR **must** reference the relevant item(s) here. Update `todo.md` after phase completion or when new work is identified.

`reports/phase0/` (with `PROGRESS.md`, templates, and ops/ batch reviews) contains the evidence machinery. Use the scripts in `scripts/phase0/` to generate commands and post-session artifacts.

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
- Provider: `--provider local|binance|binance-futures|synthetic`, `--symbol`, `--stream trade|kline_*|depth*`, `--depth-stream depth20@100ms` (for L2/queue), `--testnet`.
  - `--provider synthetic` (or `montecarlo`) generates GBM paths on the fly. Use `--mc-params "mu=0.0,sigma=0.65,n_steps=2000,initial_price=65000"` for control. All realism flags work unchanged. Can be used standalone for single synthetic paths or together with `--monte-carlo` for full campaigns.
- Monte Carlo campaigns: `--monte-carlo --mc-trials N --mc-model gbm --mc-params "..." --strategy mean-reversion`.
  - Supports `--persist` (writes campaign summary to QuestDB under the run_tag or auto-generated `mc_<ts>` tag) and `--run-tag`.
  - Outputs text summary + compact JSON. Per-trial determinism via `--seed`.
  - **Phase 5 experimental**: `--mc-parallel` runs trials concurrently using std::jthread. **Strong caveats**:
    - Conflicts with engine pinning, threading presets (recommend `--thread-preset inline`).
    - Result collection order not guaranteed (though aggregates are correct).
    - Not recommended for production validation yet.
  - Full example and limitations are documented in the Monte Carlo section below.
- Futures extras: `--margin-type isolated|cross`, `--margin-type-strict`, `--liquidation-warn-pct`, risk caps (`--max-notional`, `--max-leverage`, `--min-liq-distance-pct`), DMS (`--dead-man-countdown-ms 30000 --dead-man-heartbeat-ms 8000 --disarm-deadman`), kill (`--kill-switch-deadline-ms 5000`).
- Credentials: env `TRUETEST_BINANCE_*` (preferred; argv leaks to ps), `--api-key/--api-secret` (warns).
- Strategy: `--strategy sma,mean-reversion,structure-continuation,adaptive-hybrid`, `--param key=value` (multi-strategy comma-separated). Full list via `--help` or `StrategyRegistry`.
- Risk/portfolio: `--initial-cash`, `--risk-fraction`, `--sl-atr`, `--tp-atr`, `--max-daily-loss`, `--max-trades-per-hour`, `--risk-unwind 0.4`, `--reconcile-tolerance-bps`.
- Realism (backtest/shadow only; bypassed in live): `--order-latency-us N --order-latency-stddev-us M`, `--impact-k-bps --impact-adv`, `--walked-book-impact`, `--fill-prob/--fill-fade/--fill-decay` (probabilistic limit-fill model, default off), `--mm-levels/--mm-base-depth/--mm-spread-pct/--mm-vol-mult/--mm-max-spread-pct` (synthetic-book calibration — the seeded book is the sole source of spread cost), `--queue-model l2-snapshot` (shadow + depth-stream), `--maker-queue-model uniform|front|back` (paper + depth-stream; uniform recommended default). Deprecated warn-noops: `--realistic-fills` (passive-side fill pricing is always on), `--bar-spread-bps` (calibrate `--mm-spread-pct` instead).
- Threading: `--thread-preset inline|light|standard|full|extended` (auto from cores), `--spin-policy adaptive|spin|yield`, `--no-pin`, `--seed`.
- **Presets** (new, see P1 work): `--preset futures-phase0|mc-robustness|backtest-local-l2|shadow-tape` — named bundles that supply realistic groups of flags (DMS/risk caps for phase0, reuse+gbm for MC, L2 queue models + walked-book impact for backtests, etc.). Explicit flags and `--config` always win. Use with scripts and in the interactive TUI quick-start menu.
- Persistence: `--persist --run-tag myrun_YYYYMMDD_HHMM` (QuestDB), `--checkpoint path`.
- Replay/Record: `--replay events.bin --replay-from/--to`, `--record`, `--replay-data`.
- Output: `--output results.json`, `--status-format tui|ndjson|minimal`, `--log-events`, `--log-rotation`.

**TUI**: Rich ncurses tabbed dashboard on shadow/live (positions, orders, L2, risk, brackets, debug StageTimer/ring, health/DMS counter). Hotkeys, setup menu on backtest. `--no-tui` for headless/CI.

**JSON config**: Full engine_config schema (mode, provider, strategy, risk, threading, persistence, realism, etc.). See instructions §13 for keys.

The launcher scripts and many numbered examples in instructions §29–35 (backtest minimal → full futures live with DMS/persist/risk caps → sanitizers → PGO training → replay → QuestDB queries, etc.).

---

## 7. Providers, Data Sources, Realism Models, Orderbook (consolidated)

**Providers** (`IProvider`):
- `local`: CSV OHLCV (bar) or tick-level; BinaryCache decorator; multi-path.
- `binance` / `binance-futures`: Combined trade + depth WS, REST execution (HybridExecutor paper/shadow, signed REST + user-data WS live), L2 seeding for realism when `--depth-stream`.
- Replay: binary event log or `--replay-data`.
- Future: `drift` (see upcoming plans).

**Data validation + formats**: Strict schema checks; see instructions §19.

**Realism models** (`docs/architecture/realism.md` planned — current summary: opt-in models default off, require `--depth-stream` for L2-dependent, **completely bypassed in live**; live venue supplies truth):
- **Fill pricing (always on, no flag)**: every fill records the resting counterparty's price, one fill_event per level walked. `market_aggression` (default 1.1) is purely a crossing guarantee — never a recorded price. The deprecated `--realistic-fills` / `--bar-spread-bps` are accepted as warn-noops.
- **Synthetic book calibration** (`--mm-levels` 10, `--mm-base-depth` 100, `--mm-spread-pct` 0.002, `--mm-vol-mult` 0.25, `--mm-max-spread-pct` 0.05): in bar mode the MarketMaker's seeded ladder is the sole source of spread cost for taker fills — calibrate it to the target market. Level i rests at mid × (1 ± i × min(spread_pct + vol × vol_mult, max_spread_pct)), depth = base_depth × i. The MM pulls and re-places its quotes each bar (no stale depth accumulation); resting strategy limits fill as maker orders when a quote update crosses their level, **or when the bar's [low, high] range trades through their level** (intrabar traversal sweep — fill at the order's own limit price, so a wick through a resting limit is never silently missed).
- **Stop fills**: stops trigger on bar high/low and fill anchored at the stop price — or at the bar **open** when the bar gaps through the stop — never at the close (that would be intra-bar look-ahead). Pending strategy orders drain against a book re-centered at the bar open (next-bar-open convention holds on gap bars). **ExitManager bracket fires use the same anchoring**: SL fills at the SL level (or gap open), TP fills at the TP level (resting-limit semantics, or better at the gap open), executed against a book re-centered at the fire price within the trigger bar — never deferred to the next bar's open.
- **Intra-bar ambiguity**: OHLC bars carry no path. ExitManager resolves SL-vs-TP worst-case (SL first when both extremes cross in one bar), and tests trailing stops at their **pre-bar** level — this bar's favorable extreme raises the trail only for subsequent bars (assuming the high printed before the low would be look-ahead). Tick data / recorded-WS replay are the escape hatch for path-sensitive strategies.
- **Bracket sizing across partial fills**: an opener that walks multiple book levels emits one fill per level; the armed bracket grows with each partial (entry reference = VWAP across opener fills) so the exit always covers the whole position.
- **Probabilistic limit fills** (`--fill-prob`, `--fill-fade`, `--fill-decay`; default off): RealisticFillModel gates each limit submit with probability prob × exp(−decay × distance-from-mid); fade shaves every fill's quantity. Models adverse selection on top of the book mechanics.
- Latency: two layers (`latency_model` strategy→eligible, `wire_latency_model` order→venue). **No sub-bar latency by default** — bar-granularity delay (`--exec-bar-delay 1`) is the honest default; expect shadow-vs-backtest divergence on latency-sensitive strategies until `--order-latency-us`/`--wire-latency-us` are calibrated (REST submit round-trips ~10–50 ms are a reasonable starting point for Binance spot).
- Impact: SquareRootImpactModel raises the market-order reference before aggression — observable through which depth the crossing limit reaches (recorded prices always come from resting levels). `--walked-book-impact` uses the real L2 walked VWAP as reference instead, when depth is present.
- Queue: `--queue-model l2-snapshot` (shadow L2SnapshotQueueModel for adverse-selection honesty), `--maker-queue-model uniform|front|back` (QueueAwareBookAdapter + IQueueModel for paper/backtest maker fills; tracks size_ahead; real prints consume front; L2 shrinkage = cancels per model).

**Orderbook**: price-time priority matching; FillModel for partials; walked-book impact when L2 present.

**Futures vs Spot differences**: See futures-order-lifecycle.md table (one-way, reduceOnly brackets non-atomic, funding, liquidation math, position recon from /positionRisk, etc.).

---

## 8. Strategies, Exits, Brackets, Full Order Lifecycle (futures-order-lifecycle.md + exits/)

Strategies self-register via `REGISTER_STRATEGY` (sma, mean-reversion, ma-crossover, breakout/coiled-spring, adaptive-hybrid, structure-continuation + indicators sma/ema/rsi/bollinger/ema_regime/stochastic/swing_detector). Multi-strategy support. Emit `order_event` + `exit_intent` vectors.

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

**Ring drop policy**: `block` in backtest (deterministic backpressure — the event loop waits for the worker instead of losing events, so threaded-preset results are identical to inline); `halt_on_drop` on safety-critical rings in shadow/live. The `extended` preset's async MarketMakerWorker is shadow/live-only — backtests use the inline replenish path so book state never depends on thread scheduling.

---

## 11. QuestDB Persistence (instructions §23 + db.md full authoritative)

**Build**: `-DENABLE_QUESTDB=ON` (raw POSIX sockets; zero new runtime deps).

**Runtime**: `--persist --run-tag myrun` (validated chars). Supports `--questdb-flush-ms` (default 150) for time-based ILP flushing during long runs. Soft warning + continue (disabled) if daemon unreachable at start. Hard-fail (`--persist-strict`) is future work (see questdb-multi-week-hardening-guide.md).

**Current as-built implementation** (db.md + questdb-multi-week-hardening-guide.md): Direct calls to `QuestdbStore` (mutex-protected) from engine capture points + periodic `maybe_questdb_tick()` from main run loops (200 ms reporting blocks) for time-based flushing. `IlpWriter` handles buffering + reconnect. See Phase 1 of the hardening guide for details.

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

## 15. Operator SOPs, Testnet Guides, Demo Workflows (see governance + reports/phase0/)

See `prod.md` (Phase 0 command template + full ritual + "why each element" + Go-Live Gate) + root `todo.md` (P0-01..P0-04 + D-02; current 0/15; Phase 0 collection was paused during priority work on the monte-carlo branch — MC changes did not alter P0 gates or the live safety surface) + `reports/phase0/` (operational evidence home) + `scripts/phase0/`.

**Planned** (Doc Phases 1-2; current details live in prod.md / reports/phase0/ per CLAUDE rule): `docs/operations/futures-testnet.md`, `docs/operations/futures-phase0-operator-sop.md` (printable; P0-03), `docs/operations/demo-trading-workflow.md`, and related under `docs/operations/`. `docs/architecture/` files (target-architecture.md etc. — Doc Phase 2 / D-03) also planned; see `docs/README.md` for realized vs. aspirational map.

Tiny-size Phase 0 mainnet under conservative caps + full artifacts + two-person batch sign-off is the current gate.

**futures-testnet.md** (USDT-M): Account one-way mode, `--testnet`, math-captcha skipped, pre-trade caps + liquidation projection math, DMS details + 5-scenario playbook, refusal modes table, what engine does (recon, kill, brackets), gotchas (thin book, resets, partial brackets declined), integration smoke test.

**testnet.md** (spot counterpart): Simpler; WAF client_order_id guard; reconciler reset tolerance for monthly wipes; pointer to futures doc.

**demo-trading-workflow.md**: Exact record (`--record --persist --run-tag`), replay, shadow commands (futures variants with `--wire-latency-us`), QuestDB inspection, acceptance criteria (determinism, bounded divergence, P&L tracking). Futures adaptations box.

**futures-order-lifecycle.md**: 10-step narrative + differences vs spot + failure catchers table. "User-data WS is source of truth after initial ack." "Brackets on futures are not atomic." "DMS protects against sudden death (but does not close positions)."

**killswitch-lan-unplug-timeline.md**: Brutally honest postmortem of physical network partition. Timeline table (t=0 to >70s), what actually protected (DMS orders-only), implications for drills, recommended test procedure with `--dms-attempt-position-close` + independent machine, open gaps (blocking connect, no external watchdog yet).

**user-manual.md**: Primary overview + architecture diagram + risk/safety features + full CLI examples + limitations + file index.

**grok.md**: Implementation log for dual-portfolio shadow P&L (status, design, future phases).

---

## 16. Phase 0 Evidence Collection (reports/phase0/*)

See `reports/phase0/README.md` (layout + 6-step ritual summary), `PROGRESS.md` (master tracker; 0/15 qualifying), `templates/phase0-session-note.md` (printable signed form), and `scripts/phase0/` (new-session.sh etc. that target dated subdirs under reports/phase0/).

**Current P0 status & items**: See root `todo.md` (P0-01..P0-04; Phase 0 collection was paused during priority work on the monte-carlo branch — MC changes did not alter P0 gates or the live safety surface) + `prod.md` (full authoritative ritual + template + exit criteria).

**Planned**: `docs/operations/futures-phase0-operator-sop.md` (printable SOP; Doc Phase 1; current details in prod.md + reports/phase0/). `PHASE0_COMPLETION_PLAN.md` and some ops/ templates referenced in older notes — current operational home is reports/phase0/ + scripts/phase0/.

**Volatility regime labeling** + batch reviews every 5 with two signatures required (see reports/phase0/PROGRESS.md + todo P0-02).

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

**Phase 0 Qualifying Session Ritual & Evidence** (see `prod.md` for the authoritative template + full ritual + "why each element"; see `reports/phase0/` (README + PROGRESS.md + templates/phase0-session-note.md) + `scripts/phase0/` (new-session.sh, post-session.sh, volatility-classifier.sh) for operational machinery. Current details live in prod.md + reports/phase0/; printable SOP planned for `docs/operations/futures-phase0-operator-sop.md` — Doc Phase 1).

**Current P0 items & status**: See root `todo.md` (P0-01..P0-04; 0/15 qualifying; Phase 0 collection was paused during priority work on the monte-carlo branch — MC changes did not alter P0 gates or the live safety surface).

**Pre-Merge Safety & Phase 1 Freeze Procedure**: See `prerequisites.md` (full living checklist) + `CLAUDE.md` (model selection + token/CCB rules) + root `todo.md` (P1-01..P1-05 + frozen files list + process). Run `./scripts/check-live-safety-freeze.sh --check-head`; PR must contain `LIVE_SAFETY_CCB_APPROVED` + todo refs + "prod.md impact" note; 4h/8h clean mainnet shadow required before merge.

**Go-Live Gate**: All 9 rows require two signatures + concrete evidence (see `prod.md` for the table; "No capital tier increase is permitted until...").

**Other checklists** (DMS validation, network partition drill, QuestDB health, volatility + batch review, perf locking): See `prod.md`, `prerequisites.md`, root `todo.md` (S-*, H-*, P0-*, D-*), `scripts/`, and `reports/phase0/`. Long-form lives in governance docs per CLAUDE extraction rule.

---

## 21. Master List of Critical Warnings & Non-Negotiables (consolidated from all)

- Never increase the author's personal capital tier without prior phase exits + full Go-Live Gate sign-offs (and only for the author's tiny attended personal experiments).
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

See root governance (especially `CLAUDE.md` "Documentation Maintenance Rules", `prod.md`, `prerequisites.md`, and the new consolidated `todo.md`) for the authoritative process:
- Root gov files + reports/phase0/ + CLAUDE are the single source of truth for phases, gates, checklists, tasks, AI/reviewer rules, and evidence.
- Every frozen-surface PR must reference relevant `todo.md` items + run the check script + carry `LIVE_SAFETY_CCB_APPROVED`.
- On phase exit (declared in `prod.md`): update `todo.md` (move/strike + follow-ups), prereq if evolved, last-updated notes.
- Aspirational cross-refs *must* use explicit language: "Planned for Doc Phase X – current details live in prod.md / instructions.md §N".
- Extraction rule (CLAUDE): long-form phases/ritual/gates in `prod.md` (or dedicated SOP); this file = pointers + quick templates + MC/CLI/safety how-to.
- Anti-rot ritual before any personal capital tier increase (when the author chooses to pursue it): "docs verified + links resolve + `todo.md` updated".
- On MC/simulation landings: update MC section here + gov mentions (README, todo, prod, CLAUDE) in same PR.
- If broken/stale cross-ref: treat as doc bug.

**Current realized layout**: See `docs/README.md`. Historical material under `docs/archive/` (e.g. questdb hardening log moved here post-consolidation). Phase 0 evidence strictly in `../reports/phase0/`.

**This consolidation (2026)**: Scattered action lists / todo sections / duplicate P0/P1/MC/R/S/D details purged from docs/ files into single root `todo.md`. See root `todo.md` (D-06 + all current P0/MC/R/S/A/H items) for the full list of documentation hygiene tasks. Minor sync lags (9-vs-10 files, gaps status, planned SOP refs) noted and being cleaned.

---

**End of Master Consolidated Instructions.** All content from the original corpus has been read, deeply analyzed by multiple agents with extended cross-referenced thinking, and unified here for completeness. Use this document for the author's personal research, development, private use decisions, and safety hygiene only. For the absolute latest code state, always verify against HEAD + the enforcement scripts.

*Generated 2026-05 via parallel subagent synthesis of the full Markdown corpus.*

---

## Monte Carlo Simulation

**Current branch note**: Introduced on the `monte-carlo` branch. Now available in mainline in all three binaries (`engine_backtest`, `engine_shadow`, `engine_live`). This is a backtest / research / risk-distribution capability and does not affect live-order safety surfaces or Phase 0/1 capital gates.

TrueTest includes a first-class Monte Carlo engine for stochastic backtesting.

### Basic Usage
```bash
./build/engine_backtest \
  --monte-carlo \
  --mc-trials 500 \
  --mc-model gbm \
  --mc-params "n_steps=2000,mu=0.08,sigma=0.65,initial_price=65000" \
  --strategy mean-reversion \
  --seed 42 \
  --persist --run-tag mc_btc_500
```

This runs 500 independent GBM paths, executes your strategy on each, and prints:
- Per-trial and aggregate P&L / Sharpe / max DD statistics
- Compact JSON summary (machine readable)
- QuestDB summary row (when `--persist` is used)

### Key Flags
- `--mc-trials N` — number of paths (required for MC mode)
- `--mc-model gbm` — generator (only gbm implemented; ou listed for future)
- `--mc-params "key=val,..."` — generator parameters (n_steps, mu, sigma, etc.)
- `--mc-parallel` — **experimental** (Phase 5) concurrent trials (see warnings below)
- All normal realism flags (`--order-latency-us`, `--impact-k-bps`, `--mm-*`, etc.) and `--strategy` (any registered strategy, including structure-continuation / adaptive-hybrid) are respected per trial.

### Parallel Execution (Phase 5)
```bash
... --mc-parallel --thread-preset inline
```

**Strong warnings** (printed at runtime):
- Conflicts with engine core pinning and most threading presets.
- Use `--thread-preset inline` for safety.
- Result ordering inside aggregates is not deterministic.
- Not yet recommended for production risk analysis.

Future versions may add better thread-pool control and deterministic parallel RNG partitioning.

### Reproducibility
- `--seed` sets the master seed.
- Every trial receives a deterministic derived seed (`base_seed ^ (trial_id * magic)`).
- Same binary + same flags + same seed = bit-identical aggregates (when not using `--mc-parallel`).

### Limitations (current)
- Synthetic L2 is stylized (constant spread + noise; sufficient for basic queue/impact testing but not a full orderbook replay).
- No automatic parameter calibration from historical data yet.
- Parallel mode (`--mc-parallel`) has strong caveats (see above); result ordering is non-deterministic.
- QuestDB integration currently writes only a lightweight campaign summary row (per-trial full lifecycle capture is future work; combine with per-trial `--run-tag` for now).

### Performance Notes
- **Object reuse** (`--mc-reuse-objects`): Reuses `data_handler`, strategies, and many expensive engine-internal structures (`portfolio`, `Analytics`, `ExitManager`, `OrderTracker`, `RiskManager`, orderbooks, caches, etc.) between trials instead of full reconstruction. This significantly reduces per-trial overhead.
  - Status: Good enough for real speedups on most workloads. Full bit-identical results across every internal detail (especially engine-internal caches and order trackers) are not guaranteed.
- Win rate and profit factor (mean/median + % of trials with PF > 1) are now reported for every campaign (MC-02). Useful for stochastic robustness analysis.
- Batch path generation (`generate_batch`) is used for cache efficiency.
- **Parallel execution** (`--mc-parallel`): On a 16-core machine with `--thread-preset inline`, can deliver substantial wall-time speedup on CPU-bound strategies for large N. Strong caveats apply (see Parallel Execution section above).
- Recommended combination for maximum throughput: `--mc-reuse-objects --mc-parallel --thread-preset inline`.
- Always measure with your specific strategy and realism settings, as gains vary significantly with workload.

See `src/simulation/` (and `src/providers/synthetic/`) for the `IMonteCarloGenerator`, `MonteCarloController`, `GBMGenerator`, `SyntheticProvider`, and `MonteCarloReporter` implementations. Tests live in `tests/test_monte_carlo_*.cpp`.
