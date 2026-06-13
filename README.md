# TrueTest (hft-engine)

A modular, high-performance C++23 trading engine designed for reproducible backtesting, divergence-aware shadow trading, and gated live execution from a single source tree.

The build produces three binaries — `engine_backtest`, `engine_shadow`, `engine_live` — that differ only by the compile-time `TT_TARGET` define. Live order submission is physically impossible in the non-live binaries thanks to dead-code elimination.

**Intended Use & Scope**: TrueTest is a private, personal research and retail tool for the author only. It is not, and will never be, an enterprise-ready, institutional, or production trading system. Monte Carlo simulation, high-fidelity backtesting, and shadow divergence analysis are the primary mature capabilities. The live execution paths (`engine_live`) exist with unusually strong compile-time (`TT_TARGET`) and runtime safety layers (reconciler, DMS, kill-switch, venue risk checks, terminal halt, user-data source of truth, etc.). Any use of live paths is experimental, tiny-size, fully attended by the operator, and done at the author's own risk. The Phase 0/1 rituals and Go-Live language in this repository describe the author's personal evidence-gathering hygiene and self-imposed discipline — they are **not** a formal production release process or claim of readiness for others.

## Current Development Status (2026, post-merge)

Monte Carlo simulation capabilities were integrated from the `monte-carlo` feature branch (now part of mainline). The merge of the monte-carlo branch into master was completed successfully following the full gated process in MERGE_PLAN.md (all phases 0-8 passed).

**Major new capability** (stochastic backtesting & risk analysis):
- **Monte Carlo simulation engine** — full multi-trial campaigns with GBM (and future models) path generation, deterministic per-trial seeding, object reuse between trials for performance, and experimental parallel execution. Usable both as standalone synthetic paths (`--provider synthetic`) and as full campaigns (`--monte-carlo --mc-trials N`). Strong caveats apply: stylized synthetic L2, no automatic calibration from historical data, parallel mode has threading/pinning restrictions (recommended with `--thread-preset inline`). This is a research and strategy-robustness tool; it does not change the live-order safety surface or Phase 0/1 capital gates.

**Recent major advancements** (deepdive stabilization & realism pass):
- **Queue-position awareness** (6-step implementation) — realistic orderbook queue modeling, latency, impact, and walked-book fill simulation for high-fidelity shadow vs. reality divergence tracking.
- Modernized **rich ncurses TUI** with tabbed panels, risk dashboards, and live updates (multiple UI passes).
- **Opt-in web UI** (`-DENABLE_WEB=ON` + `--web`) — a read-only browser cockpit + backtest-review SPA served by an embedded civetweb HTTP/WS server, reading the same `snapshot_dashboard()` seam as the TUI (off the hot path, no order routes). See [`docs/web-ui.md`](docs/web-ui.md).
- **Dead Man’s Switch** hardening — now attempts position flattening (`reduceOnly` MARKET) on heartbeat loss, plus related safety optimizations.
- **Tiered maintenance-margin risk** (Phase 2.2) — `MaintenanceMarginTable` + `FuturesRiskCheck` using real `/fapi/v1/leverageBracket` data; funding events wired toward P&L and risk.
- Extensive **realism wiring**, per-lot bookkeeping, hybrid executor, ExitManager, and orderbook fidelity improvements.
- UI cleanup, comment stripping, and documentation synchronization across CLAUDE.md / instructions.md.

**Production-readiness gating** (see [`prod.md`](prod.md)):
- **Phase 0 — Safe Tiny-Size Mainnet Futures**: 0/15 qualifying as of 2026. Phase 0 collection was paused during priority work on the monte-carlo branch (gates/ritual unchanged). Collecting 15+ qualifying sessions across volatility regimes with zero unexplained drift. Full artifacts (binary log + QuestDB + notes) required. Tracker in [`reports/phase0/`](reports/phase0/); current ritual + template in [`prod.md`](prod.md); printable SOP planned in `docs/operations/futures-phase0-operator-sop.md` (Doc Phase 1; explicit "current details live in prod.md" per CLAUDE rules).
- **Phase 1 — Live-Safety Freeze**: Planning artifacts created, `LIVE-SAFETY SURFACE` markers applied to the 10 frozen files (exact list in `scripts/check-live-safety-freeze.sh`, `prerequisites.md`, `prod.md`, `CLAUDE.md`; tt_target.h, engine.cpp, futures provider live block + dead_mans/kill/reconciler, risk_manager, futures_risk_check, live_safety, worker_watchdog), enforcement script (`scripts/check-live-safety-freeze.sh`) wired into CI + pre-commit, CLAUDE.md + prerequisites policy updated. Awaiting final clean 8-hour mainnet `engine_shadow` run + two-person sign-off. (Note: some older refs say "9 critical files"; standardized on the 10-file list.)
- Strong safety primitives (valuable even for careful private use) already in place: compile-time live-order gating, IReconciler + clock-skew checks, layered DMS + kill-switch, venue-specific `IRiskCheck`, terminal `halt_flag_`, user-data WebSocket as source-of-truth, WorkerWatchdog.
- **Current recommendation**: Tiny-size validation and research only. Not suitable for meaningful capital or unattended operation until Phase 0/1 exit criteria are satisfied. Full gap analysis in [`docs/production-readiness-gaps.md`](docs/production-readiness-gaps.md).

The engine has excellent Binance spot + USDT-M futures support (signed REST, combined trade + depth20 streams, brackets, OCO, funding, reconciler) and a clean pluggable `IProvider` architecture. Future venue work (e.g. Solana Drift liquidation keeper) is tracked in `upcoming/`.

## Authoritative Documentation

| Document                                              | Purpose |
|-------------------------------------------------------|---------|
| [`CLAUDE.md`](CLAUDE.md)                              | **Single source of truth** for the *current* codebase: build flags & matrix, directory layout, coding conventions, hot-path rules, model-selection guidance, live-safety freeze policy. |
| [`prod.md`](prod.md)                                  | Production readiness playbook, exact phase definitions, capital-tier gates, Phase 0 command template + operator SOP, Go-Live checklist. |
| [`docs/user-manual.md`](docs/user-manual.md)          | Comprehensive technical + operator manual (architecture, safety, TUI, realism models). |
| [`docs/instructions.md`](docs/instructions.md)        | Exhaustive CLI flag reference, CMake options, provider modes, QuestDB, threading, replay, etc. |
| [`todo.md`](todo.md) + (planned: `docs/migration.md`, `docs/target-architecture.md`) | Deepdive task list + chronological architecture history (architecture/ files planned for Doc Phase 2). |
| [`reports/phase0/PROGRESS.md`](reports/phase0/PROGRESS.md) | Live tracker of Phase 0 qualifying sessions. |
| [`prerequisites.md`](prerequisites.md)                | Mandatory checklist before any new work touches the frozen safety surface. |
| summary.md (root) + CLAUDE.md (for AI rules) + this MERGE_PLAN.md context | Dense power + constraints briefing **for AI agents** (current state, MC capabilities, invariants, model-selection rules; old ENGINE summary consolidated here in Phase 4). Read first when acting as coding assistant. |

Additional deep-dive notes (realism.md, futures-order-lifecycle.md, etc.) are planned under `docs/architecture/` and `docs/operations/` — see the approved doc plan in `todo.md` (D- items) and `docs/README.md`. Current authoritative details live in `prod.md`, `CLAUDE.md`, and `instructions.md`.

## Quick Build & Run

```bash
# Default — CSV backtesting only, zero external runtime dependencies
cmake -B build
cmake --build build

./build/engine_backtest --provider local --path market_data.csv --strategy sma
```

Optional backends (enable at configure time):

```bash
cmake -B build \
  -DENABLE_BINANCE=ON \     # Binance spot + futures (REST + WS)
  -DENABLE_QUESTDB=ON \     # High-resolution persistence + ILP
  -DENABLE_WEB=ON \         # Embedded web UI server (civetweb; --web) — docs/web-ui.md
  -DENABLE_LIVE_DATA=ON \   # Generic WebSocket data source
  -DENABLE_DEBUG=ON \       # Instrumentation & stage timers
  -DENABLE_NATIVE_OPT=ON    # -march=native for Release builds
cmake --build build
```

**Futures shadow example** (requires the two flags above):

```bash
./build/engine_shadow \
  --provider binance-futures \
  --symbol BTCUSDT \
  --stream trade --depth-stream depth20@100ms \
  --persist --run-tag demo_$(date +%Y%m%d_%H%M)
```

Live execution (`engine_live`) adds `--live --api-key … --api-secret …` and a mandatory math-captcha confirmation on mainnet. Full templates and risk caps are documented in `prod.md` (Phase 0 section).

## Data Formats & Providers

- Standard OHLCV CSV for classic backtests.
- Tick-level CSV and live WebSocket feeds via the `local`, `binance`, and `binance-futures` providers (see [`docs/instructions.md`](docs/instructions.md) for exact schemas).
- Synthetic / Monte Carlo path generation via the `synthetic` provider (GBM paths on demand; see the Monte Carlo section in [`docs/instructions.md`](docs/instructions.md)).
- Full replay from binary event logs (`--replay`).

## Testing & Quality

Large GoogleTest suite (`test_*`), golden-file regression for execution fidelity, CI-enforced hot-path rules (no JSON), live-safety-freeze script, and optional sanitizers + benchmarks.

---

This README is deliberately concise. All operational detail, architecture decisions, current invariants, and the full development log live in `CLAUDE.md` and the `docs/` tree. The project follows a strict phase-gated approach to production readiness (for the author's private use only) — consult `prod.md` before increasing live capital. The monte-carlo branch has been fully merged into master (post-merge, 2026).